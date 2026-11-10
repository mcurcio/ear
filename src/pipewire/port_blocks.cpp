// Port blocks as pw_filter nodes (docs/engine.md, port_blocks.hpp): every block in EAR's graph group, an asynchronous
// drive applied on the block's data loop, and level metering of the public ports in windows aligned on the graph clock.
// Each cycle's GraphCycle is made here, both clock domains sampled once (docs/engine.md#graphcycle), and latency is
// carried per bank pair under PW_FILTER_FLAG_CUSTOM_LATENCY. The asynchronous drive and fade
// (docs/engine.md#drive-and-fade): requests in a ring of eight, one invoke in flight per block, applied inside it,
// completed through the block's own event source. The ring companions ride in each drive, bound with the code in its
// invoke: each source (docs/engine.md#sources) is played into the block's own scratch for its inside input bank before
// the code, told the downstream latency the block last told on that bank, and each sink's tap (docs/engine.md#sinks) is
// written after the code and the fade, stamped with the upstream latency the block last told on its bank; both every
// cycle that code runs. The fade's ramp, each cycle's discontinuity and both stamps are core's block rules
// (ear/core/block_rules.hpp), which the type harness's block runs too.
#include "ear/pipewire/port_blocks.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include "callback_boundary.hpp"
#include "ear/core/block_rules.hpp"
#include "ear/pipewire/data_loop_wedge.hpp"
#include "engine_names.hpp"
#include "engine_property_set.hpp"
#include "latency_spa.hpp"
#include "level_mailbox.hpp"

namespace ear::pipewire {
namespace {

/// One block as latestLevels sees it: its name, its public ports (inputs then outputs) and the
/// mailbox its realtime side publishes into.
struct RegisteredBlock {
	RegisteredBlock(NodeName name, std::vector<PortName> metered)
		: node(std::move(name)), ports(std::move(metered)), mailbox(ports.size()) {}
	NodeName node;
	std::vector<PortName> ports;
	LevelMailbox mailbox;
};

/// What every block of one factory shares. Blocks hold it too, so a block that outlives its
/// factory still leaves the registry safely.
struct Levels {
	explicit Levels(LevelWake& bell) : wake(bell) {}
	LevelWake& wake;
	/// The window the bell was last rung for: the first block to publish a window rings `wake`
	/// once for it, and the blocks after it see the window already rung. Only the realtime side
	/// touches it, so no reader has to answer it: a bell answered without reading the levels
	/// (the control port with no levels subscriber) never silences later windows.
	std::atomic<std::uint64_t> rungFor{0};
	/// Guards `blocks` and every mailbox's read side. Taken on the main loop (make, destroy) and
	/// by latestLevels' caller, never on the realtime thread.
	std::mutex mutex;
	std::vector<RegisteredBlock*> blocks;
};

std::vector<PortName> publicPorts(const Ports& ports) {
	std::vector<PortName> names = ports.inputs();
	names.insert(names.end(), ports.outputs().begin(), ports.outputs().end());
	return names;
}

std::int64_t nowNs(clockid_t clock) noexcept {
	timespec now{};
	::clock_gettime(clock, &now);
	return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

/// A downstream latency as the realtime side reads it: least frames in the high half, most in the low.
std::uint64_t packed(const Latency& latency) noexcept {
	return (static_cast<std::uint64_t>(latency.minFrames) << 32U) | latency.maxFrames;
}
Latency unpacked(std::uint64_t word) noexcept {
	return Latency{static_cast<std::uint32_t>(word >> 32U), static_cast<std::uint32_t>(word & 0xFFFFFFFFU)};
}

class FilterPortBlock final : public PortBlock {
public:
	FilterPortBlock(Engine& engine, std::shared_ptr<Levels> levels, const NodeName& node, const Ports& ports,
	                const AudioFormat& format, PortBlockEvents& events)
		: engine_(engine), events_(events), loop_(engine.mainLoop()), maxBlock_(format.maxBlock),
		  windowFrames_(std::max(levelWindowFrames(format.rate), 1U)), zeros_(format.maxBlock, 0.0f),
		  inputs_(ports.inputs().size() + ports.insideInputs().size()),
		  outputs_(ports.outputs().size() + ports.insideOutputs().size()), publicInputs_(ports.inputs().size()),
		  publicOutputs_(ports.outputs().size()), meters_(ports.inputs().size() + ports.outputs().size()),
		  levels_(std::move(levels)), registered_(node, publicPorts(ports)), ports_(ports), rate_(format.rate),
		  downstream_(outputs_.size()), downstreamNow_(outputs_.size()), inputToldNow_(inputs_.size()),
		  outputToldNow_(outputs_.size()), inputPeers_(inputs_.size()), outputPeers_(outputs_.size()),
		  inputTold_(inputs_.size()), outputTold_(outputs_.size()), ramp_(format.rate), fadeMask_(outputs_.size(), 0),
		  sinks_(insideBanks(ports, Direction::output)), tapSpans_(outputs_.size()),
		  sources_(insideBanks(ports, Direction::input)), sourceSpans_(inputs_.size()) {
		for (std::size_t i = 0; i < outputs_.size(); ++i) {
			scratch_.emplace_back(format.maxBlock, 0.0f);
		}
		// A source plays into scratch of the block's own, never into PipeWire's buffer of its (unlinked) port: one
		// buffer per inside input port.
		for (std::size_t i = publicInputs_; i < inputs_.size(); ++i) sourceScratch_.emplace_back(format.maxBlock, 0.0f);
		{
			const std::scoped_lock lock(levels_->mutex);
			levels_->blocks.push_back(&registered_);
		}
		refusal_ = addEvent(loop_, guarded<&FilterPortBlock::onRefusal>, this);
		completion_ = addEvent(loop_, guarded<&FilterPortBlock::onCompletion>, this);
		// Every key from the property table (docs/engine.md#properties-ear-sets says why each).
		filter_ = pw_filter_new(engine.core(), node.c_str(),
		                        PropertySet<Home::block>({{property<Home::block>("node.name"), node},
		                                                  {property<Home::block>("media.name"), node},
		                                                  {property<Home::block>("media.type"), "Audio"},
		                                                  {property<Home::block>("node.group"), graphGroup},
		                                                  {property<Home::block>("node.always-process"), "true"}})
		                            .release());
		if (filter_ == nullptr) {
			fail(std::string("cannot make the engine node: ") + spa_strerror(-errno));
			return;
		}
		listen<FilterCallbacks<FilterPortBlock>{.stateChanged = guarded<&FilterPortBlock::onStateChanged>,
		                                        .paramChanged = guarded<&FilterPortBlock::onParamChanged>,
		                                        .process = realtime<&FilterPortBlock::onProcess>}>(filter_, listener_,
		                                                                                           this);
		// AudioCode::process sees inputs then inside inputs, outputs then inside outputs.
		addPorts(PW_DIRECTION_INPUT, ports.inputs(), inputPorts_);
		addPorts(PW_DIRECTION_INPUT, ports.insideInputs(), inputPorts_);
		addPorts(PW_DIRECTION_OUTPUT, ports.outputs(), outputPorts_);
		addPorts(PW_DIRECTION_OUTPUT, ports.insideOutputs(), outputPorts_);
		// Custom latency: pw_filter would otherwise merge every output's downstream latency into every input (and
		// every input's upstream into every output), mixing paths that never meet (filter.c, default_latency). The
		// block carries it per bank pair instead (carryLatency).
		if (const int res = pw_filter_connect(
				filter_, static_cast<pw_filter_flags>(PW_FILTER_FLAG_RT_PROCESS | PW_FILTER_FLAG_CUSTOM_LATENCY),
				nullptr, 0);
		    res < 0) {
			fail(std::string("cannot connect the engine node: ") + spa_strerror(res));
		}
	}

	// Removing the listener first keeps the disconnect inside pw_filter_destroy from being told
	// as a loss; it also clears the process callback on the data loop (a blocking invoke), and
	// pw_filter_destroy removes the node from that loop, so process() is never called after
	// either returns. Only then does the block leave the registry, so nothing writes a mailbox
	// that latestLevels can no longer reach, and nothing reads one that is freed.
	//
	// An invoke of this block's still queued (posted, not yet run) would run on a freed block, so one blocking no-op
	// invoke on the loop it was posted to flushes it first (invokes run in posting order). Only then is the completion
	// source destroyed, so a completion for a destroyed block runs nothing; requests not completed are dropped.
	~FilterPortBlock() override {
		if (filter_ != nullptr) {
			spa_hook_remove(&listener_);
			pw_filter_destroy(filter_);
		}
		if (postedLoop_ && invokesPosted_ != invokesRun_.load(std::memory_order_acquire)) {
			invoke(postedLoop_, realtime<&FilterPortBlock::onNothing>, this, true);
		}
		if (completion_ != nullptr) pw_loop_destroy_source(loop_, completion_);
		if (refusal_ != nullptr) pw_loop_destroy_source(loop_, refusal_);
		const std::scoped_lock lock(levels_->mutex);
		std::erase(levels_->blocks, &registered_);
	}

	FilterPortBlock(const FilterPortBlock&) = delete;
	FilterPortBlock& operator=(const FilterPortBlock&) = delete;

	void drive(DriveRequest request, std::move_only_function<void(std::uint64_t)> done) override {
		// A sink reads an inside output bank, a source plays into an inside input bank, and NodeHost gives each bank at
		// most one companion, so a drive binds at most one tap per such bank: the realtime storage is sized for that at
		// make, and apply() binds no more than it holds. Nothing is thrown here: this runs on the main loop, inside
		// PipeWire's C callbacks.
		pending_.push_back(
			Pending{Kind::drive, request.code, request.faded, request.sinks, request.sources, std::move(done), {}});
		pump();
	}

	void fadeOut(std::move_only_function<void()> done) override {
		pending_.push_back(Pending{Kind::fade, nullptr, {}, {}, {}, {}, std::move(done)});
		pump();
	}

	/// Runs the `done` of every request the data loop completed, in order, then hands the ring what waits behind it
	/// and posts the next invoke if one is due. A `done` may make new requests (they queue behind) or destroy the
	/// block (nothing of it is touched afterwards).
	void settle() override {
		const std::weak_ptr<int> alive = life_;
		const std::uint64_t completed = completed_.load(std::memory_order_acquire);
		while (finished_ < completed) {
			Pending next = std::move(pending_.front());
			pending_.pop_front();
			const Request& slot = ring_[finished_ % kRing];
			const std::uint64_t taken = slot.takenAt;
			const bool cancelled = slot.cancelled;
			++finished_;
			if (next.kind == Kind::drive) {
				if (next.driven) next.driven(taken);
			} else if (!cancelled && next.faded) {
				next.faded();
			}
			if (alive.expired()) return;
		}
		pump();
	}

	std::uint64_t takenAt() const noexcept override { return takenAt_.load(std::memory_order_acquire); }

	std::uint32_t blockId() const noexcept override { return blockId_; }

	void carryLatency(const LatencyPlan& plan) override {
		// Worked out here first, so a plan that does not fit the ports throws to its caller and is never kept: after
		// this only the peers change, so no later retell, inside a peer's param_changed, meets a bad plan.
		const LatencyFacts facts = workOut(ports_, plan, inputPeers_, outputPeers_);
		plan_ = plan;
		tell(facts);
	}

private:
	/// How many inside banks of a direction the ports have: the most sinks (output) or sources (input) a drive binds.
	static std::size_t insideBanks(const Ports& ports, Direction direction) {
		return static_cast<std::size_t>(std::ranges::count_if(ports.banks(), [direction](const Ports::Bank& bank) {
			return bank.reach == PortBank::Reach::inside && bank.direction == direction;
		}));
	}

	void addPorts(pw_direction direction, const std::vector<PortName>& names, std::vector<void*>& handles) {
		// The handle is the port's (empty) user data, freed with the filter; a refused port (null)
		// leaves its span on zeros or scratch.
		for (const PortName& name : names) {
			handles.push_back(pw_filter_add_port(
				filter_, direction, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
				PropertySet<Home::blockPort>({{property<Home::blockPort>("format.dsp"), "32 bit float mono audio"},
			                                  {property<Home::blockPort>("port.name"), name}})
					.release(),
				nullptr, 0));
		}
	}

	/// A refusal while making the block is told on the main loop, never inside make().
	void fail(std::string detail) {
		refusalDetail_ = std::move(detail);
		if (refusal_ != nullptr) pw_loop_signal_event(loop_, refusal_);
	}

	void onRefusal(std::uint64_t /*count*/) {
		if (lostTold_) return;
		lostTold_ = true;
		events_.lost(refusalDetail_);
	}

	/// ERROR, or UNCONNECTED after a connect (the engine removed the node: filter.c's
	/// proxy_removed), is a loss. The block's own destroy removes this listener first.
	void onStateChanged(pw_filter_state old, pw_filter_state state, const char* error) {
		// Whether the block processes, for the data loop's fades; leaving STREAMING acknowledges a fade still pending,
		// by one more invoke, since no cycle may come to end it.
		const bool streaming = state == PW_FILTER_STATE_STREAMING;
		processing_.store(streaming, std::memory_order_release);
		if (streaming && blockId_ == SPA_ID_INVALID && filter_ != nullptr) {
			blockId_ = pw_filter_get_node_id(filter_);
		}
		if (!streaming && old == PW_FILTER_STATE_STREAMING && completed_.load(std::memory_order_acquire) < pushed_) {
			kick_ = true;
			pump();
		}
		if (state == PW_FILTER_STATE_STREAMING && !readyTold_) {
			readyTold_ = true;
			events_.ready();
		} else if (state == PW_FILTER_STATE_ERROR && !lostTold_) {
			lostTold_ = true;
			events_.lost(error != nullptr ? error : "engine error");
		} else if (state == PW_FILTER_STATE_UNCONNECTED && old != PW_FILTER_STATE_UNCONNECTED && !lostTold_) {
			lostTold_ = true;
			events_.lost(error != nullptr ? error : "the engine removed the block");
		}
	}

	/// Main thread: a latency the linked ports report on one of the block's ports. pw_filter tells only the far
	/// side's (an input's upstream, an output's downstream) and, under custom latency, does nothing else with it. A
	/// port that has heard one has a peer, in frames from here on (latency_spa.hpp); a null param (none) leaves it with
	/// none, so its bank's latency leaves it out.
	void onParamChanged(void* port, std::uint32_t id, const spa_pod* param) {
		if (id != SPA_PARAM_Latency || port == nullptr) return;
		const auto heard = [&](const std::vector<void*>& ports, std::vector<std::optional<Latency>>& peers,
		                       spa_direction farSide) {
			const auto found = std::ranges::find(ports, port);
			if (found == ports.end()) return false;
			const auto index = static_cast<std::size_t>(found - ports.begin());
			if (param == nullptr) {
				peers[index] = std::nullopt;
				return true;
			}
			spa_latency_info info{};
			if (spa_latency_parse(param, &info) < 0 || info.direction != farSide) return true;
			peers[index] = fromSpa(info, maxBlock_, rate_);
			return true;
		};
		if (heard(inputPorts_, inputPeers_, SPA_DIRECTION_OUTPUT) ||
		    heard(outputPorts_, outputPeers_, SPA_DIRECTION_INPUT)) {
			tell(workOut(ports_, plan_, inputPeers_, outputPeers_));
		}
	}

	/// Main thread: publishes what the model worked out (docs/engine.md#latency-per-bank-pair) to the realtime side,
	/// each output's downstream and each port's told latency, and tells each port's new told latency to its peers, in
	/// rate units.
	void tell(const LatencyFacts& facts) {
		for (std::size_t i = 0; i < downstream_.size(); ++i)
			downstream_[i].store(packed(facts.downstream[i]), std::memory_order_release);
		// On a wedged data loop no latency is told to the peers (data_loop_wedge.hpp): nothing more reaches the
		// engine. What the realtime side reads is still published: it is only atomics.
		const bool tellPeers = filter_ != nullptr && !dataLoopWedged(engine_);
		// What each port tells, for the realtime side too: an output's capture latency stamps its sink's chunks, an
		// input's playback latency dates what its source played.
		const auto publish = [&](std::size_t base, const std::vector<void*>& ports,
		                         std::vector<std::atomic<std::uint64_t>>& toldNow, std::vector<Latency>& last,
		                         spa_direction side) {
			for (std::size_t i = 0; i < toldNow.size(); ++i) {
				const Latency told = facts.told[base + i];
				toldNow[i].store(packed(told), std::memory_order_release);
				if (!tellPeers || i >= ports.size() || ports[i] == nullptr || last[i] == told) continue;
				last[i] = told;
				const spa_latency_info info = toSpa(told, side);
				std::uint8_t buffer[256];
				spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
				const spa_pod* params[1] = {
					static_cast<const spa_pod*>(spa_latency_build(&builder, SPA_PARAM_Latency, &info))};
				pw_filter_update_params(filter_, ports[i], params, 1);
			}
		};
		publish(0, inputPorts_, inputToldNow_, inputTold_, SPA_DIRECTION_INPUT);
		publish(inputToldNow_.size(), outputPorts_, outputToldNow_, outputTold_, SPA_DIRECTION_OUTPUT);
	}

	// ---------------------------------------------------------------------------------------------------------------
	// The asynchronous drive and fade (docs/engine.md#drive-and-fade). Request n sits in ring_[n % kRing] from its push
	// until the main thread ran its `done`; the main thread pushes only into a slot whose request it finished, and
	// publishes what it pushed (published_, release); the data loop drains up to that and writes each request's result
	// into its slot before it publishes the request completed (completed_, release). Requests complete in order: a
	// drive at once, in the invoke; a fade-out when a cycle ended at zero, or at once on a block not processing, or
	// (cancelled) when a later drive is applied. So the requests drained but not completed are always fade-outs, all
	// pending.

	/// Main thread: moves what waits into the ring while it has room, then posts an invoke if none is in flight and
	/// the ring holds what the data loop has not drained (or a fade must be acknowledged after leaving STREAMING).
	/// Never runs a `done`.
	void pump() {
		while (pushed_ < finished_ + pending_.size() && pushed_ - finished_ < kRing) {
			const Pending& next = pending_[pushed_ - finished_];
			ring_[pushed_ % kRing] = Request{.kind = next.kind,
			                                 .code = next.code,
			                                 .faded = next.mask.data(),
			                                 .fadedCount = next.mask.size(),
			                                 .sinks = next.sinks.data(),
			                                 .sinkCount = next.sinks.size(),
			                                 .sources = next.sources.data(),
			                                 .sourceCount = next.sources.size()};
			++pushed_;
		}
		published_.store(pushed_, std::memory_order_release);
		if (invokesPosted_ != invokesRun_.load(std::memory_order_acquire)) return; // its completion pumps again
		if (drained_.load(std::memory_order_acquire) == pushed_ && !kick_) return;
		kick_ = false;
		const DataLoop dataLoop = filter_ != nullptr ? DataLoop::of(filter_) : DataLoop();
		if (!dataLoop) {
			// Nothing processes: applied here, and still completed by signal.
			apply();
			pw_loop_signal_event(loop_, completion_);
			return;
		}
		// One invoke in flight per block. A loop whose thread is not running runs it inline: it only sets
		// data-loop state and signals, never runs a `done`.
		postedLoop_ = dataLoop;
		++invokesPosted_;
		invoke(dataLoop, realtime<&FilterPortBlock::onRequests>, this, false);
	}

	/// Data loop, between two cycles: applies every request the ring holds, then says the invoke ran and signals.
	void onRequests() noexcept {
		apply();
		invokesRun_.fetch_add(1, std::memory_order_acq_rel);
		pw_loop_signal_event(loop_, completion_);
	}

	/// The data loop: nothing, so a blocking invoke of it flushes the invokes posted before it.
	void onNothing() noexcept {}

	/// Data loop (or the main thread of a block with no data loop): drains the ring.
	void apply() noexcept {
		const std::uint64_t until = published_.load(std::memory_order_acquire);
		std::uint64_t drained = drained_.load(std::memory_order_relaxed);
		std::uint64_t completed = completed_.load(std::memory_order_relaxed);
		for (; drained < until; ++drained) {
			Request& request = ring_[drained % kRing];
			if (request.kind == Kind::fade) {
				request.cancelled = false;
				ramp_.fadeOut();
				continue;
			}
			for (std::uint64_t pending = completed; pending < drained; ++pending)
				ring_[pending % kRing].cancelled = true;
			code_.store(request.code, std::memory_order_release);
			// The taps change with the code, in this invoke: copied into storage sized at make.
			sinkCount_ = std::min(request.sinkCount, sinks_.size());
			std::copy_n(request.sinks, sinkCount_, sinks_.begin());
			sourceCount_ = std::min(request.sourceCount, sources_.size());
			std::copy_n(request.sources, sourceCount_, sources_.begin());
			continuity_.driven(); // the new code's first cycle is a discontinuity to it
			bool anyFaded = false;
			for (std::size_t i = 0; i < fadeMask_.size(); ++i) {
				fadeMask_[i] = i < request.fadedCount && request.faded[i] ? 1 : 0;
				anyFaded = anyFaded || fadeMask_[i] != 0;
			}
			ramp_.driven(anyFaded);
			request.takenAt = continuity_.nextPosition();
			takenAt_.store(continuity_.nextPosition(), std::memory_order_release);
			completed = drained + 1;
		}
		drained_.store(drained, std::memory_order_release);
		if (completed < drained && !processing_.load(std::memory_order_acquire)) {
			ramp_.silenced(); // no cycle will come to end it
			completed = drained;
		}
		completed_.store(completed, std::memory_order_release);
	}

	/// Realtime thread, after the code: the fade's ramp on the faded outputs (FadeRamp), and a fade-out acknowledged at
	/// the end of the first cycle that ended at zero.
	void applyFade(std::uint32_t frames) noexcept {
		if (ramp_.apply(outputs_, fadeMask_, frames).outEnded) acknowledgeFade();
	}

	/// Realtime thread: every pending fade-out is done.
	void acknowledgeFade() noexcept {
		const std::uint64_t drained = drained_.load(std::memory_order_relaxed);
		if (completed_.load(std::memory_order_relaxed) == drained) return;
		completed_.store(drained, std::memory_order_release);
		pw_loop_signal_event(loop_, completion_);
	}

	/// Main loop: the data loop completed requests, or an invoke ran.
	void onCompletion(std::uint64_t /*count*/) { settle(); }

	/// Realtime thread: run the code (or silence), meter the public ports, and publish the
	/// window when this cycle crosses a window boundary of the graph clock.
	void onProcess(spa_io_position* position) noexcept {
		const spa_io_clock& clock = position->clock;
		const auto frames = static_cast<std::uint32_t>(clock.duration);
		// The clock moved on by the last cycle's frames on the same driver, or it did not (CycleContinuity's rule).
		const CycleContinuity::Step step = continuity_.next(clock.id, clock.position, frames, maxBlock_);
		if (step.runs) {
			// The driver's clock.nsec is CLOCK_MONOTONIC even while it tracks the raw clock
			// (docs/engine.md#the-internal-clock); both clocks are read together here, so a late wake cancels out. The
			// raw read sits between two monotonic ones, and the offset is taken from their midpoint, so a preemption
			// between reads skews it by half at most (node-driver.c, get_nsec_offset).
			const std::int64_t monoBefore = nowNs(CLOCK_MONOTONIC);
			const std::int64_t raw = nowNs(CLOCK_MONOTONIC_RAW);
			const std::int64_t monoAfter = nowNs(CLOCK_MONOTONIC);
			const std::int64_t mono = monoBefore + (monoAfter - monoBefore) / 2;
			const auto startNs = static_cast<std::int64_t>(clock.nsec);
			for (std::size_t i = 0; i < downstreamNow_.size(); ++i)
				downstreamNow_[i] = unpacked(downstream_[i].load(std::memory_order_acquire));
			processCycle(frames, GraphCycle{.startNs = startNs,
			                                .position = clock.position,
			                                .startRawNs = startNs + (raw - mono),
			                                .rateDiff = clock.rate_diff,
			                                .driverId = clock.id,
			                                .nextNs = static_cast<std::int64_t>(clock.next_nsec),
			                                .xruns = continuity_.xruns(),
			                                .discontinuity = step.discontinuity,
			                                .downstream = downstreamNow_});
		} else {
			for (void* port : outputPorts_) {
				if (port == nullptr) continue;
				if (auto* out = static_cast<float*>(pw_filter_get_dsp_buffer(port, frames))) {
					std::fill_n(out, frames, 0.0f);
				}
			}
			// It wrote zeros: a fade-out ends here.
			if (ramp_.fadingOut()) {
				ramp_.silenced();
				acknowledgeFade();
			}
		}
		// Every block of one driver sees the same clock position, so every block's windows end at
		// the same multiples of windowFrames_, whenever the block was made.
		const std::uint64_t start = position->clock.position;
		const std::uint64_t boundary = (start + frames) / windowFrames_;
		if (boundary != start / windowFrames_) publish(boundary * windowFrames_);
	}

	void processCycle(std::uint32_t frames, const GraphCycle& cycle) noexcept {
		for (std::size_t i = 0; i < inputPorts_.size(); ++i) {
			const auto* in = inputPorts_[i] == nullptr
			                     ? nullptr
			                     : static_cast<const float*>(pw_filter_get_dsp_buffer(inputPorts_[i], frames));
			inputs_[i] = std::span<const float>(in != nullptr ? in : zeros_.data(), frames);
		}
		for (std::size_t i = 0; i < outputPorts_.size(); ++i) {
			auto* out = outputPorts_[i] == nullptr
			                ? nullptr
			                : static_cast<float*>(pw_filter_get_dsp_buffer(outputPorts_[i], frames));
			outputs_[i] = std::span<float>(out != nullptr ? out : scratch_[i].data(), frames);
		}
		// Each source, before the code, into the block's scratch for its inside input bank, which the code then reads
		// in place of the bank's ports; only while a code runs: a silence binds none.
		for (std::size_t s = 0; s < sourceCount_; ++s) {
			const SourceBinding& binding = sources_[s];
			if (binding.tap == nullptr || binding.channels == 0 || binding.firstInput < publicInputs_ ||
			    binding.firstInput + binding.channels > inputs_.size())
				continue;
			for (std::size_t c = 0; c < binding.channels; ++c)
				sourceSpans_[c] =
					std::span<float>(sourceScratch_[binding.firstInput + c - publicInputs_].data(), frames);
			const Latency downstream =
				unpacked(inputToldNow_[binding.firstInput].load(std::memory_order_acquire)); // the bank's told
			binding.tap->play(std::span<const std::span<float>>(sourceSpans_.data(), binding.channels),
			                  sourceCycle(cycle, frames, downstream));
			for (std::size_t c = 0; c < binding.channels; ++c) inputs_[binding.firstInput + c] = sourceSpans_[c];
		}
		if (AudioCode* code = code_.load(std::memory_order_acquire)) {
			code->process(inputs_, outputs_, frames, cycle);
		} else {
			for (const std::span<float> out : outputs_) std::ranges::fill(out, 0.0f);
		}
		applyFade(frames); // before metering, so levels show the fade
		// Each sink's tap, after the code and the fade (a sink's bank is never faded), from its inside output bank's
		// spans (an unlinked one's scratch included); only while a code runs: a silence binds none.
		for (std::size_t s = 0; s < sinkCount_; ++s) {
			const SinkBinding& binding = sinks_[s];
			if (binding.tap == nullptr || binding.channels == 0 ||
			    binding.firstOutput + binding.channels > outputs_.size())
				continue;
			for (std::size_t c = 0; c < binding.channels; ++c) tapSpans_[c] = outputs_[binding.firstOutput + c];
			const Latency upstream =
				unpacked(outputToldNow_[binding.firstOutput].load(std::memory_order_acquire)); // the bank's told
			binding.tap->write(std::span<const std::span<const float>>(tapSpans_.data(), binding.channels), frames,
			                   chunkStamp(cycle, upstream));
		}
		// Public ports only: inputs_ and outputs_ list the public ports before the inside ones.
		for (std::size_t i = 0; i < publicInputs_; ++i) meters_[i].add(inputs_[i]);
		for (std::size_t i = 0; i < publicOutputs_; ++i) meters_[publicInputs_ + i].add(outputs_[i]);
	}

	void publish(std::uint64_t windowEnd) noexcept {
		const std::span<Level> next = registered_.mailbox.next();
		for (std::size_t i = 0; i < meters_.size(); ++i) next[i] = meters_[i].take();
		registered_.mailbox.publish(windowEnd);
		if (levels_->rungFor.exchange(windowEnd, std::memory_order_acq_rel) != windowEnd) {
			levels_->wake.levelsPublished();
		}
	}

	std::atomic<AudioCode*> code_{nullptr}; // nullptr: every output silent
	Engine& engine_;
	PortBlockEvents& events_;
	pw_loop* loop_;
	std::uint32_t maxBlock_;
	std::uint32_t windowFrames_;
	std::vector<float> zeros_;                // an unlinked input reads these
	std::vector<std::vector<float>> scratch_; // an output with no buffer this cycle writes here
	std::vector<std::span<const float>> inputs_;
	std::vector<std::span<float>> outputs_;
	std::vector<void*> inputPorts_;
	std::vector<void*> outputPorts_;
	std::size_t publicInputs_;
	std::size_t publicOutputs_;
	std::vector<LevelMeter> meters_; // public inputs, then public outputs (realtime thread)
	std::shared_ptr<Levels> levels_;
	RegisteredBlock registered_;
	Ports ports_;
	std::uint32_t rate_;
	CycleContinuity continuity_; // the clock as the realtime side last saw it; data loop only
	// Latency: each output's downstream, published by the main thread for the realtime side (packed), and the copy
	// a cycle hands its code; then, on the main thread only, what each port's peers report, what each port told them
	// last, and the run's plan, from which workOut works the rest out.
	std::vector<std::atomic<std::uint64_t>> downstream_;
	std::vector<Latency> downstreamNow_;
	// What each port last told its peers, in frames (packed), published by the main thread for the realtime side: an
	// input's playback latency, an output's capture latency.
	std::vector<std::atomic<std::uint64_t>> inputToldNow_;
	std::vector<std::atomic<std::uint64_t>> outputToldNow_;
	static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
	              "the realtime side reads packed latencies lock-free");
	std::vector<std::optional<Latency>> inputPeers_;  // upstream of each input; none: no peer reported
	std::vector<std::optional<Latency>> outputPeers_; // downstream of each output; none: no peer reported
	std::vector<Latency> inputTold_;                  // what each input last told its peers (zero: nothing yet)
	std::vector<Latency> outputTold_;
	LatencyPlan plan_;
	pw_filter* filter_ = nullptr;
	spa_hook listener_{};
	bool readyTold_ = false;
	bool lostTold_ = false;
	spa_source* refusal_ = nullptr;
	std::string refusalDetail_;

	// The asynchronous drive and fade (see pump()).
	enum class Kind : std::uint8_t { drive, fade };
	static constexpr std::size_t kRing = 8; // docs/design/companions.md: a first choice
	/// One request in the ring: what the main thread hands, then what the data loop writes back.
	struct Request {
		Kind kind = Kind::drive;
		AudioCode* code = nullptr;
		const bool* faded = nullptr;
		std::size_t fadedCount = 0;
		const SinkBinding* sinks = nullptr; // a drive: the taps it binds
		std::size_t sinkCount = 0;
		const SourceBinding* sources = nullptr; // a drive: the sources it binds
		std::size_t sourceCount = 0;
		std::uint64_t takenAt = 0; // a drive: where it took effect
		bool cancelled = false;    // a fade-out: a later drive cancelled it
	};
	/// One request on the main thread, from drive() or fadeOut() until its `done` ran.
	struct Pending {
		Kind kind;
		AudioCode* code;
		std::span<const bool> mask;
		std::span<const SinkBinding> sinks;
		std::span<const SourceBinding> sources;
		std::move_only_function<void(std::uint64_t)> driven;
		std::move_only_function<void()> faded;
	};
	FadeRamp ramp_;                                 // data loop only
	std::vector<std::uint8_t> fadeMask_;            // per output; data loop only
	std::vector<SinkBinding> sinks_;                // the applied drive's taps, sized at make; data loop only
	std::size_t sinkCount_ = 0;                     // how many of sinks_ are bound; data loop only
	std::vector<std::span<const float>> tapSpans_;  // one tap's bank, built per cycle; data loop only
	std::vector<SourceBinding> sources_;            // the applied drive's sources, sized at make; data loop only
	std::size_t sourceCount_ = 0;                   // how many of sources_ are bound; data loop only
	std::vector<std::vector<float>> sourceScratch_; // per inside input port: what a source plays; data loop only
	std::vector<std::span<float>> sourceSpans_;     // one source's bank, built per cycle; data loop only
	std::array<Request, kRing> ring_{};
	std::atomic<std::uint64_t> published_{0}; // requests the main thread pushed into the ring
	std::atomic<std::uint64_t> drained_{0};   // requests the data loop applied
	std::atomic<std::uint64_t> completed_{0}; // requests the data loop completed
	std::atomic<std::uint64_t> takenAt_{0};
	std::atomic<bool> processing_{false}; // STREAMING, as the main loop last heard
	std::atomic<std::uint64_t> invokesRun_{0};
	// Main thread only.
	std::deque<Pending> pending_; // from request finished_ on
	std::uint64_t pushed_ = 0;
	std::uint64_t finished_ = 0;
	std::uint64_t invokesPosted_ = 0;
	DataLoop postedLoop_; // the loop the last invoke was posted to: the block's data loop
	bool kick_ = false;
	std::uint32_t blockId_ = SPA_ID_INVALID;
	spa_source* completion_ = nullptr;
	std::shared_ptr<int> life_ = std::make_shared<int>(0); // a `done` that destroyed the block expires it
};

} // namespace

struct FilterPortBlocks::Impl {
	Engine& engine;
	std::shared_ptr<Levels> levels;
};

FilterPortBlocks::FilterPortBlocks(Engine& engine, LevelWake& wake)
	: impl_(std::make_unique<Impl>(engine, std::make_shared<Levels>(wake))) {}

FilterPortBlocks::~FilterPortBlocks() = default;

std::unique_ptr<PortBlock> FilterPortBlocks::make(const NodeName& node, const Ports& ports, const AudioFormat& format,
                                                  PortBlockEvents& events) {
	return std::make_unique<FilterPortBlock>(impl_->engine, impl_->levels, node, ports, format, events);
}

/// One snapshot is one window. Blocks publish one after another within a cycle, so a read can
/// find some blocks at window N and the rest still at N - 1; it then takes N - 1 (the oldest
/// latest window) from every block, through the previous window each publish carries. A block
/// whose first window is newer than that has no window in this snapshot yet. Only a reader
/// stalled for a whole window between two blocks, or a clock that jumped (a new driver), can
/// leave a block with neither; that block reports its latest window.
std::vector<NodeLevels> FilterPortBlocks::latestLevels() {
	Levels& levels = *impl_->levels;
	const std::scoped_lock lock(levels.mutex);
	std::vector<std::pair<const RegisteredBlock*, const PublishedLevels*>> published;
	for (RegisteredBlock* block : levels.blocks) {
		if (const PublishedLevels* latest = block->mailbox.read()) {
			published.emplace_back(block, latest);
		}
	}
	if (published.empty()) return {};
	std::uint64_t windowEnd = published.front().second->latest.windowEnd;
	for (const auto& entry : published) windowEnd = std::min(windowEnd, entry.second->latest.windowEnd);
	std::vector<NodeLevels> snapshot;
	snapshot.reserve(published.size());
	for (const auto& [block, latest] : published) {
		const LevelWindow* window = &latest->latest;
		if (window->windowEnd != windowEnd) {
			if (!latest->hasPrevious) continue;
			if (latest->previous.windowEnd == windowEnd) window = &latest->previous;
		}
		NodeLevels node{block->node, window->windowEnd, {}};
		node.ports.reserve(block->ports.size());
		for (std::size_t i = 0; i < block->ports.size(); ++i) {
			node.ports.push_back(PortLevel{block->ports[i], window->levels[i]});
		}
		snapshot.push_back(std::move(node));
	}
	return snapshot;
}

} // namespace ear::pipewire
