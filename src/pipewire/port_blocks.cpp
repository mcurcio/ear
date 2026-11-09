// Port blocks as pw_filter nodes (docs/design/batch2.md section 4.3, port_blocks.hpp): every
// block in EAR's graph group, an asynchronous drive applied on the block's data loop, and level
// metering of the public ports in windows aligned on the graph clock. Each cycle's GraphCycle is
// made here, both clock domains sampled once (docs/design/node-framework.md section 9.3), and
// latency is carried per bank pair under PW_FILTER_FLAG_CUSTOM_LATENCY (section 9.4). Slice 3a's
// asynchronous drive and fade (docs/design/node-lifecycle.md section 5.3, step L-2): requests in a
// ring of eight, one invoke in flight per block, applied inside it, completed through the block's
// own event source; the blocking drive went with step L-3a.
#include "ear/pipewire/port_blocks.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include "ear/pipewire/data_loop_wedge.hpp"
#include "engine_names.hpp"
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

/// A latency of no path: zero in every unit.
spa_latency_info noLatency(spa_direction direction) {
	spa_latency_info info{};
	info.direction = direction;
	return info;
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
		  downstream_(outputs_.size()), downstreamNow_(outputs_.size()),
		  inputPeers_(inputs_.size(), noLatency(SPA_DIRECTION_OUTPUT)),
		  outputPeers_(outputs_.size(), noLatency(SPA_DIRECTION_INPUT)), inputHeard_(inputs_.size(), false),
		  outputHeard_(outputs_.size(), false), inputTold_(inputs_.size(), noLatency(SPA_DIRECTION_INPUT)),
		  outputTold_(outputs_.size(), noLatency(SPA_DIRECTION_OUTPUT)),
		  fadeFrames_(std::max(format.rate * 5 / 1000, 1U)), fadeMask_(outputs_.size(), 0) {
		for (std::size_t i = 0; i < outputs_.size(); ++i) {
			scratch_.emplace_back(format.maxBlock, 0.0f);
		}
		{
			const std::scoped_lock lock(levels_->mutex);
			levels_->blocks.push_back(&registered_);
		}
		refusal_ = pw_loop_add_event(loop_, &FilterPortBlock::onRefusal, this);
		completion_ = pw_loop_add_event(loop_, &FilterPortBlock::onCompletion, this);
		// pw_filter_new's name becomes media.name; node.name (how the registry, and so the live
		// graph, names the node) defaults to the program's name unless set here. node.group puts
		// every block in one group, so one driver clocks them all.
		filter_ = pw_filter_new(engine.core(), node.c_str(),
		                        pw_properties_new(PW_KEY_NODE_NAME, node.c_str(), PW_KEY_MEDIA_TYPE, "Audio",
		                                          PW_KEY_NODE_GROUP, graphGroup, PW_KEY_NODE_ALWAYS_PROCESS, "true",
		                                          nullptr));
		if (filter_ == nullptr) {
			fail(std::string("cannot make the engine node: ") + spa_strerror(-errno));
			return;
		}
		pw_filter_add_listener(filter_, &listener_, &filterEvents(), this);
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
		if (postedLoop_ != nullptr && invokesPosted_ != invokesRun_.load(std::memory_order_acquire)) {
			pw_loop_invoke(postedLoop_, &FilterPortBlock::onNothing, 0, nullptr, 0, true, nullptr);
		}
		if (completion_ != nullptr) pw_loop_destroy_source(loop_, completion_);
		if (refusal_ != nullptr) pw_loop_destroy_source(loop_, refusal_);
		const std::scoped_lock lock(levels_->mutex);
		std::erase(levels_->blocks, &registered_);
	}

	FilterPortBlock(const FilterPortBlock&) = delete;
	FilterPortBlock& operator=(const FilterPortBlock&) = delete;

	void drive(DriveRequest request, std::move_only_function<void(std::uint64_t)> done) override {
		pending_.push_back(Pending{Kind::drive, request.code, request.faded, std::move(done), {}});
		pump();
	}

	void fadeOut(std::move_only_function<void()> done) override {
		pending_.push_back(Pending{Kind::fade, nullptr, {}, {}, std::move(done)});
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
		for (const BankPair& pair : plan.pairs) {
			static_cast<void>(ports_.bank(pair.from)); // a pair naming no bank is a type bug, as the pass-through's
			static_cast<void>(ports_.bank(pair.to));
		}
		for (const auto& entry : plan.internal) static_cast<void>(ports_.bank(entry.first));
		plan_ = plan;
		retell();
	}

private:
	static const pw_filter_events& filterEvents() {
		static const pw_filter_events table = [] {
			pw_filter_events events{};
			events.version = PW_VERSION_FILTER_EVENTS;
			events.state_changed = &FilterPortBlock::onStateChanged;
			events.param_changed = &FilterPortBlock::onParamChanged;
			events.process = &FilterPortBlock::onProcess;
			return events;
		}();
		return table;
	}

	void addPorts(pw_direction direction, const std::vector<PortName>& names, std::vector<void*>& handles) {
		// The handle is the port's (empty) user data, freed with the filter; a refused port (null)
		// leaves its span on zeros or scratch.
		for (const PortName& name : names) {
			handles.push_back(pw_filter_add_port(filter_, direction, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			                                     pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
			                                                       PW_KEY_PORT_NAME, name.c_str(), nullptr),
			                                     nullptr, 0));
		}
	}

	/// A refusal while making the block is told on the main loop, never inside make().
	void fail(std::string detail) {
		refusalDetail_ = std::move(detail);
		if (refusal_ != nullptr) pw_loop_signal_event(loop_, refusal_);
	}

	static void onRefusal(void* data, std::uint64_t /*count*/) {
		auto* self = static_cast<FilterPortBlock*>(data);
		if (self->lostTold_) return;
		self->lostTold_ = true;
		self->events_.lost(self->refusalDetail_);
	}

	/// ERROR, or UNCONNECTED after a connect (the engine removed the node: filter.c's
	/// proxy_removed), is a loss. The block's own destroy removes this listener first.
	static void onStateChanged(void* data, pw_filter_state old, pw_filter_state state, const char* error) {
		auto* self = static_cast<FilterPortBlock*>(data);
		// Whether the block processes, for the data loop's fades; leaving STREAMING acknowledges a fade still pending,
		// by one more invoke, since no cycle may come to end it.
		const bool streaming = state == PW_FILTER_STATE_STREAMING;
		self->processing_.store(streaming, std::memory_order_release);
		if (streaming && self->blockId_ == SPA_ID_INVALID && self->filter_ != nullptr) {
			self->blockId_ = pw_filter_get_node_id(self->filter_);
		}
		if (!streaming && old == PW_FILTER_STATE_STREAMING &&
		    self->completed_.load(std::memory_order_acquire) < self->pushed_) {
			self->kick_ = true;
			self->pump();
		}
		if (state == PW_FILTER_STATE_STREAMING && !self->readyTold_) {
			self->readyTold_ = true;
			self->events_.ready();
		} else if (state == PW_FILTER_STATE_ERROR && !self->lostTold_) {
			self->lostTold_ = true;
			self->events_.lost(error != nullptr ? error : "engine error");
		} else if (state == PW_FILTER_STATE_UNCONNECTED && old != PW_FILTER_STATE_UNCONNECTED && !self->lostTold_) {
			self->lostTold_ = true;
			self->events_.lost(error != nullptr ? error : "the engine removed the block");
		}
	}

	/// Main thread: a latency the linked ports report on one of the block's ports. pw_filter tells only the far
	/// side's (an input's upstream, an output's downstream) and, under custom latency, does nothing else with it. A
	/// port that has heard one has a peer; a null param (none) leaves it with none, so its bank's latency leaves it out.
	static void onParamChanged(void* data, void* port, std::uint32_t id, const spa_pod* param) {
		auto* self = static_cast<FilterPortBlock*>(data);
		if (id != SPA_PARAM_Latency || port == nullptr) return;
		const auto heard = [&](std::vector<void*>& ports, std::vector<spa_latency_info>& peers,
		                       std::vector<bool>& reported, spa_direction farSide) {
			const auto found = std::ranges::find(ports, port);
			if (found == ports.end()) return false;
			const auto index = static_cast<std::size_t>(found - ports.begin());
			spa_latency_info info = noLatency(farSide);
			if (param != nullptr && (spa_latency_parse(param, &info) < 0 || info.direction != farSide)) return true;
			peers[index] = info;
			reported[index] = param != nullptr;
			return true;
		};
		if (heard(self->inputPorts_, self->inputPeers_, self->inputHeard_, SPA_DIRECTION_OUTPUT) ||
		    heard(self->outputPorts_, self->outputPeers_, self->outputHeard_, SPA_DIRECTION_INPUT)) {
			self->retell();
		}
	}

	/// The ports of one bank in the block's port lists.
	struct BankSpan {
		std::size_t first;
		std::size_t count;
	};
	BankSpan span(const std::string& bank) const {
		const Ports::Bank& found = ports_.bank(bank);
		return {found.first, found.ports.size()};
	}

	/// The peers of `bank`'s ports combined: what lies beyond the bank. A port with no peer is no path, so it is left
	/// out (as pw_impl_port_recalc_latency combines only a port's links), and a bank with none comes to zero.
	static spa_latency_info combined(const std::vector<spa_latency_info>& peers, const std::vector<bool>& reported,
	                                 BankSpan bank, spa_direction side) {
		spa_latency_info info{};
		spa_latency_info_combine_start(&info, side);
		for (std::size_t i = bank.first; i < bank.first + bank.count; ++i)
			if (reported[i]) spa_latency_info_combine(&info, &peers[i]);
		spa_latency_info_combine_finish(&info);
		return info;
	}

	Latency internalOf(const std::string& bank) const {
		for (const auto& [name, latency] : plan_.internal)
			if (name == bank) return latency;
		return {};
	}

	/// What a latency comes to in frames of the graph: a quantum is the block size, and ns are at the graph frequency.
	Latency frames(const spa_latency_info& info) const {
		const auto count = [&](float quantum, std::int32_t rate, std::int64_t ns) {
			const double total =
				static_cast<double>(quantum) * maxBlock_ + rate + static_cast<double>(ns) * rate_ / 1'000'000'000.0;
			return total <= 0.0 ? 0U : static_cast<std::uint32_t>(std::lround(total));
		};
		return {count(info.min_quantum, info.min_rate, info.min_ns),
		        count(info.max_quantum, info.max_rate, info.max_ns)};
	}

	/// Main thread: works out every bank's latency afresh from what the peers report and the plan, publishes each
	/// output's downstream latency to the realtime side, and tells each port's new latency to its peers.
	void retell() {
		// On a wedged data loop no latency is told (data_loop_wedge.hpp): nothing more reaches the engine.
		if (filter_ == nullptr || dataLoopWedged(engine_)) return;
		for (const Ports::Bank& bank : ports_.banks()) {
			const BankSpan at{bank.first, bank.ports.size()};
			const Latency internal = internalOf(bank.name);
			spa_latency_info told{};
			if (bank.direction == Direction::output) {
				const Latency beyond = frames(combined(outputPeers_, outputHeard_, at, SPA_DIRECTION_INPUT));
				for (std::size_t i = at.first; i < at.first + at.count; ++i)
					downstream_[i].store(packed(beyond), std::memory_order_release);
				// Upstream of an output: what reaches the paired inputs, plus the run's own.
				spa_latency_info_combine_start(&told, SPA_DIRECTION_OUTPUT);
				for (const BankPair& pair : plan_.pairs) {
					if (pair.to != bank.name) continue;
					const spa_latency_info upstream =
						combined(inputPeers_, inputHeard_, span(pair.from), SPA_DIRECTION_OUTPUT);
					spa_latency_info_combine(&told, &upstream);
				}
			} else {
				// Downstream of an input: what lies beyond the paired outputs, plus the run's own.
				spa_latency_info_combine_start(&told, SPA_DIRECTION_INPUT);
				for (const BankPair& pair : plan_.pairs) {
					if (pair.from != bank.name) continue;
					const spa_latency_info downstream =
						combined(outputPeers_, outputHeard_, span(pair.to), SPA_DIRECTION_INPUT);
					spa_latency_info_combine(&told, &downstream);
				}
			}
			spa_latency_info_combine_finish(&told);
			told.min_rate += static_cast<std::int32_t>(internal.minFrames);
			told.max_rate += static_cast<std::int32_t>(internal.maxFrames);
			auto& ports = bank.direction == Direction::output ? outputPorts_ : inputPorts_;
			auto& last = bank.direction == Direction::output ? outputTold_ : inputTold_;
			for (std::size_t i = at.first; i < at.first + at.count; ++i) {
				if (ports[i] == nullptr || spa_latency_info_compare(&last[i], &told) == 0) continue;
				last[i] = told;
				std::uint8_t buffer[256];
				spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
				const spa_pod* params[1] = {
					static_cast<const spa_pod*>(spa_latency_build(&builder, SPA_PARAM_Latency, &told))};
				pw_filter_update_params(filter_, ports[i], params, 1);
			}
		}
	}

	// ---------------------------------------------------------------------------------------------------------------
	// The asynchronous drive and fade (node-lifecycle.md 5.3). Request n sits in ring_[n % kRing] from its push until
	// the main thread ran its `done`; the main thread pushes only into a slot whose request it finished, and publishes
	// what it pushed (published_, release); the data loop drains up to that and writes each request's result into its
	// slot before it publishes the request completed (completed_, release). Requests complete in order: a drive at
	// once, in the invoke; a fade-out when a cycle ended at zero, or at once on a block not processing, or (cancelled)
	// when a later drive is applied. So the requests drained but not completed are always fade-outs, all pending.

	/// Main thread: moves what waits into the ring while it has room, then posts an invoke if none is in flight and
	/// the ring holds what the data loop has not drained (or a fade must be acknowledged after leaving STREAMING).
	/// Never runs a `done`.
	void pump() {
		while (pushed_ < finished_ + pending_.size() && pushed_ - finished_ < kRing) {
			const Pending& next = pending_[pushed_ - finished_];
			ring_[pushed_ % kRing] = Request{next.kind, next.code, next.mask.data(), next.mask.size(), 0, false};
			++pushed_;
		}
		published_.store(pushed_, std::memory_order_release);
		if (invokesPosted_ != invokesRun_.load(std::memory_order_acquire)) return; // its completion pumps again
		if (drained_.load(std::memory_order_acquire) == pushed_ && !kick_) return;
		kick_ = false;
		pw_loop* dataLoop = filter_ != nullptr ? pw_filter_get_data_loop(filter_) : nullptr;
		if (dataLoop == nullptr) {
			// Nothing processes: applied here, and still completed by signal.
			apply();
			pw_loop_signal_event(loop_, completion_);
			return;
		}
		// One invoke in flight per block (D-L38). A loop whose thread is not running runs it inline: it only sets
		// data-loop state and signals, never runs a `done`.
		postedLoop_ = dataLoop;
		++invokesPosted_;
		pw_loop_invoke(dataLoop, &FilterPortBlock::onRequests, 0, nullptr, 0, false, this);
	}

	/// Data loop, between two cycles: applies every request the ring holds, then says the invoke ran and signals.
	static int onRequests(spa_loop* /*loop*/, bool /*async*/, std::uint32_t /*seq*/, const void* /*data*/,
	                      std::size_t /*size*/, void* userData) {
		auto* self = static_cast<FilterPortBlock*>(userData);
		self->apply();
		self->invokesRun_.fetch_add(1, std::memory_order_acq_rel);
		pw_loop_signal_event(self->loop_, self->completion_);
		return 0;
	}

	static int onNothing(spa_loop* /*loop*/, bool /*async*/, std::uint32_t /*seq*/, const void* /*data*/,
	                     std::size_t /*size*/, void* /*userData*/) {
		return 0;
	}

	/// Data loop (or the main thread of a block with no data loop): drains the ring.
	void apply() noexcept {
		const std::uint64_t until = published_.load(std::memory_order_acquire);
		std::uint64_t drained = drained_.load(std::memory_order_relaxed);
		std::uint64_t completed = completed_.load(std::memory_order_relaxed);
		for (; drained < until; ++drained) {
			Request& request = ring_[drained % kRing];
			if (request.kind == Kind::fade) {
				request.cancelled = false;
				fade_ = Fade::out;
				continue;
			}
			for (std::uint64_t pending = completed; pending < drained; ++pending)
				ring_[pending % kRing].cancelled = true;
			code_.store(request.code, std::memory_order_release);
			fresh_ = true; // the new code's first cycle is a discontinuity to it
			bool anyFaded = false;
			for (std::size_t i = 0; i < fadeMask_.size(); ++i) {
				fadeMask_[i] = i < request.fadedCount && request.faded[i] ? 1 : 0;
				anyFaded = anyFaded || fadeMask_[i] != 0;
			}
			fade_ = anyFaded ? Fade::in : Fade::none;
			fadeAt_ = 0;
			request.takenAt = nextPosition_;
			takenAt_.store(nextPosition_, std::memory_order_release);
			completed = drained + 1;
		}
		drained_.store(drained, std::memory_order_release);
		if (completed < drained && !processing_.load(std::memory_order_acquire)) {
			fadeAt_ = 0; // no cycle will come to end it
			completed = drained;
		}
		completed_.store(completed, std::memory_order_release);
	}

	/// Realtime thread, after the code: the fade's gain on the faded outputs, sample by sample (fadeAt_ / fadeFrames_,
	/// exact at both ends), and a fade-out acknowledged at the end of the first cycle that ended at zero.
	void applyFade(std::uint32_t frames) noexcept {
		if (fade_ == Fade::none) return;
		for (std::uint32_t i = 0; i < frames; ++i) {
			const float gain = static_cast<float>(fadeAt_) / static_cast<float>(fadeFrames_);
			for (std::size_t o = 0; o < outputs_.size(); ++o) {
				if (fadeMask_[o] != 0) outputs_[o][i] = fadeAt_ == 0 ? 0.0f : outputs_[o][i] * gain;
			}
			if (fade_ == Fade::in && fadeAt_ < fadeFrames_) {
				++fadeAt_;
			} else if (fade_ == Fade::out && fadeAt_ > 0) {
				--fadeAt_;
			}
		}
		if (fade_ == Fade::in && fadeAt_ == fadeFrames_) fade_ = Fade::none;
		if (fade_ == Fade::out && fadeAt_ == 0) acknowledgeFade();
	}

	/// Realtime thread: every pending fade-out is done.
	void acknowledgeFade() noexcept {
		const std::uint64_t drained = drained_.load(std::memory_order_relaxed);
		if (completed_.load(std::memory_order_relaxed) == drained) return;
		completed_.store(drained, std::memory_order_release);
		pw_loop_signal_event(loop_, completion_);
	}

	/// Main loop: the data loop completed requests, or an invoke ran.
	static void onCompletion(void* data, std::uint64_t /*count*/) { static_cast<FilterPortBlock*>(data)->settle(); }

	/// Realtime thread: run the code (or silence), meter the public ports, and publish the
	/// window when this cycle crosses a window boundary of the graph clock.
	static void onProcess(void* data, spa_io_position* position) {
		auto* self = static_cast<FilterPortBlock*>(data);
		const spa_io_clock& clock = position->clock;
		const auto frames = static_cast<std::uint32_t>(clock.duration);
		// The clock moved on by the last cycle's frames on the same driver, or it did not: a new driver, a resync.
		const bool sameDriver = self->cycles_ > 0 && clock.id == self->lastDriver_;
		// A new code's first cycle, and the cycle after one too large to run, are discontinuities to the code too.
		const bool discontinuity =
			!sameDriver || clock.position != self->nextPosition_ || self->fresh_ || self->skipped_;
		if (sameDriver && clock.position > self->nextPosition_) ++self->xruns_;
		self->fresh_ = false;
		self->skipped_ = frames > self->maxBlock_;
		++self->cycles_;
		self->lastDriver_ = clock.id;
		self->nextPosition_ = clock.position + clock.duration;
		if (frames <= self->maxBlock_) {
			// The driver's clock.nsec is CLOCK_MONOTONIC even while it tracks the raw clock (docs/design/node-framework.md
			// section 10); both clocks are read together here, so a late wake cancels out. The raw read sits between two
			// monotonic ones, and the offset is taken from their midpoint, so a preemption between reads skews it by
			// half at most (node-driver.c, get_nsec_offset).
			const std::int64_t monoBefore = nowNs(CLOCK_MONOTONIC);
			const std::int64_t raw = nowNs(CLOCK_MONOTONIC_RAW);
			const std::int64_t monoAfter = nowNs(CLOCK_MONOTONIC);
			const std::int64_t mono = monoBefore + (monoAfter - monoBefore) / 2;
			const auto startNs = static_cast<std::int64_t>(clock.nsec);
			for (std::size_t i = 0; i < self->downstreamNow_.size(); ++i)
				self->downstreamNow_[i] = unpacked(self->downstream_[i].load(std::memory_order_acquire));
			self->processCycle(frames, GraphCycle{.startNs = startNs,
			                                      .position = clock.position,
			                                      .startRawNs = startNs + (raw - mono),
			                                      .rateDiff = clock.rate_diff,
			                                      .driverId = clock.id,
			                                      .nextNs = static_cast<std::int64_t>(clock.next_nsec),
			                                      .xruns = self->xruns_,
			                                      .discontinuity = discontinuity,
			                                      .downstream = self->downstreamNow_});
		} else {
			for (void* port : self->outputPorts_) {
				if (port == nullptr) continue;
				if (auto* out = static_cast<float*>(pw_filter_get_dsp_buffer(port, frames))) {
					std::fill_n(out, frames, 0.0f);
				}
			}
			// It wrote zeros: a fade-out ends here.
			if (self->fade_ == Fade::out) {
				self->fadeAt_ = 0;
				self->acknowledgeFade();
			}
		}
		// Every block of one driver sees the same clock position, so every block's windows end at
		// the same multiples of windowFrames_, whenever the block was made.
		const std::uint64_t start = position->clock.position;
		const std::uint64_t boundary = (start + frames) / self->windowFrames_;
		if (boundary != start / self->windowFrames_) self->publish(boundary * self->windowFrames_);
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
		if (AudioCode* code = code_.load(std::memory_order_acquire)) {
			code->process(inputs_, outputs_, frames, cycle);
		} else {
			for (const std::span<float> out : outputs_) std::ranges::fill(out, 0.0f);
		}
		applyFade(frames); // before metering, so levels show the fade
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
	// The clock as the realtime side last saw it.
	std::uint64_t cycles_ = 0;
	std::uint32_t lastDriver_ = 0;
	std::uint64_t nextPosition_ = 0;
	std::uint64_t xruns_ = 0;
	bool fresh_ = false;   // drive() installed a code since the last cycle (data loop only)
	bool skipped_ = false; // the last cycle was too large to run
	// Latency: each output's downstream, published by the main thread for the realtime side (packed), and the copy
	// a cycle hands its code; then, on the main thread only, what each port's peers report, what each port told them
	// last, and the run's plan.
	std::vector<std::atomic<std::uint64_t>> downstream_;
	std::vector<Latency> downstreamNow_;
	std::vector<spa_latency_info> inputPeers_;  // upstream of each input
	std::vector<spa_latency_info> outputPeers_; // downstream of each output
	std::vector<bool> inputHeard_;              // whether each input has a peer that reported
	std::vector<bool> outputHeard_;
	std::vector<spa_latency_info> inputTold_;
	std::vector<spa_latency_info> outputTold_;
	LatencyPlan plan_;
	pw_filter* filter_ = nullptr;
	spa_hook listener_{};
	bool readyTold_ = false;
	bool lostTold_ = false;
	spa_source* refusal_ = nullptr;
	std::string refusalDetail_;

	// The asynchronous drive and fade (see pump()).
	enum class Kind : std::uint8_t { drive, fade };
	enum class Fade : std::uint8_t { none, in, out };
	static constexpr std::size_t kRing = 8; // node-lifecycle.md 16: a first choice
	/// One request in the ring: what the main thread hands, then what the data loop writes back.
	struct Request {
		Kind kind = Kind::drive;
		AudioCode* code = nullptr;
		const bool* faded = nullptr;
		std::size_t fadedCount = 0;
		std::uint64_t takenAt = 0; // a drive: where it took effect
		bool cancelled = false;    // a fade-out: a later drive cancelled it
	};
	/// One request on the main thread, from drive() or fadeOut() until its `done` ran.
	struct Pending {
		Kind kind;
		AudioCode* code;
		std::span<const bool> mask;
		std::move_only_function<void(std::uint64_t)> driven;
		std::move_only_function<void()> faded;
	};
	std::uint32_t fadeFrames_;
	std::vector<std::uint8_t> fadeMask_; // per output; data loop only
	Fade fade_ = Fade::none;             // data loop only
	std::uint32_t fadeAt_ = 0;           // the fade's position, 0 (silent) to fadeFrames_ (full); data loop only
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
	pw_loop* postedLoop_ = nullptr;
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
