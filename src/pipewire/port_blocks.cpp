// Port blocks as pw_filter nodes (docs/design/batch2.md section 4.3, port_blocks.hpp): every
// block in EAR's graph group, drive() as a barrier with the block's data loop, and level
// metering of the public ports in windows aligned on the graph clock. Each cycle's GraphCycle is
// made here, both clock domains sampled once (docs/design/node-framework.md section 9.3), and
// latency is carried per bank pair under PW_FILTER_FLAG_CUSTOM_LATENCY (section 9.4).
#include "ear/pipewire/port_blocks.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
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
		: events_(events), loop_(engine.mainLoop()), maxBlock_(format.maxBlock),
		  windowFrames_(std::max(levelWindowFrames(format.rate), 1U)), zeros_(format.maxBlock, 0.0f),
		  inputs_(ports.inputs().size() + ports.insideInputs().size()),
		  outputs_(ports.outputs().size() + ports.insideOutputs().size()), publicInputs_(ports.inputs().size()),
		  publicOutputs_(ports.outputs().size()), meters_(ports.inputs().size() + ports.outputs().size()),
		  levels_(std::move(levels)), registered_(node, publicPorts(ports)), ports_(ports), rate_(format.rate),
		  downstream_(outputs_.size()), downstreamNow_(outputs_.size()),
		  inputPeers_(inputs_.size(), noLatency(SPA_DIRECTION_OUTPUT)),
		  outputPeers_(outputs_.size(), noLatency(SPA_DIRECTION_INPUT)),
		  inputTold_(inputs_.size(), noLatency(SPA_DIRECTION_INPUT)),
		  outputTold_(outputs_.size(), noLatency(SPA_DIRECTION_OUTPUT)) {
		for (std::size_t i = 0; i < outputs_.size(); ++i) {
			scratch_.emplace_back(format.maxBlock, 0.0f);
		}
		{
			const std::scoped_lock lock(levels_->mutex);
			levels_->blocks.push_back(&registered_);
		}
		refusal_ = pw_loop_add_event(loop_, &FilterPortBlock::onRefusal, this);
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
	~FilterPortBlock() override {
		if (filter_ != nullptr) {
			spa_hook_remove(&listener_);
			pw_filter_destroy(filter_);
		}
		if (refusal_ != nullptr) pw_loop_destroy_source(loop_, refusal_);
		const std::scoped_lock lock(levels_->mutex);
		std::erase(levels_->blocks, &registered_);
	}

	FilterPortBlock(const FilterPortBlock&) = delete;
	FilterPortBlock& operator=(const FilterPortBlock&) = delete;

	/// The new code is stored on the block's data loop, between two cycles, and the call waits
	/// for that: when it returns, no process() of the old code is running or will run. A block
	/// with no data loop (refused, never connected, or its local node destroyed: filter.c clears
	/// the pointer) is not processing, so storing the pointer is enough. A block whose engine
	/// node was destroyed through the registry keeps its data loop (E1's test found this), so
	/// the invoke runs there and returns as usual.
	void drive(AudioCode* code) override {
		pw_loop* dataLoop = filter_ != nullptr ? pw_filter_get_data_loop(filter_) : nullptr;
		if (dataLoop == nullptr) {
			code_.store(code, std::memory_order_release);
			return;
		}
		pw_loop_invoke(dataLoop, &FilterPortBlock::onDrive, 0, &code, sizeof(code), true, this);
	}

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
	/// side's (an input's upstream, an output's downstream) and, under custom latency, does nothing else with it.
	static void onParamChanged(void* data, void* port, std::uint32_t id, const spa_pod* param) {
		auto* self = static_cast<FilterPortBlock*>(data);
		if (id != SPA_PARAM_Latency || port == nullptr) return;
		const auto heard = [&](std::vector<void*>& ports, std::vector<spa_latency_info>& peers, spa_direction farSide) {
			const auto found = std::ranges::find(ports, port);
			if (found == ports.end()) return false;
			spa_latency_info info = noLatency(farSide);
			if (param != nullptr && (spa_latency_parse(param, &info) < 0 || info.direction != farSide)) return true;
			peers[static_cast<std::size_t>(found - ports.begin())] = info;
			return true;
		};
		if (heard(self->inputPorts_, self->inputPeers_, SPA_DIRECTION_OUTPUT) ||
		    heard(self->outputPorts_, self->outputPeers_, SPA_DIRECTION_INPUT)) {
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

	/// Every port of `bank`'s peers combined: what lies beyond the bank.
	static spa_latency_info combined(const std::vector<spa_latency_info>& peers, BankSpan bank, spa_direction side) {
		spa_latency_info info{};
		spa_latency_info_combine_start(&info, side);
		for (std::size_t i = bank.first; i < bank.first + bank.count; ++i) spa_latency_info_combine(&info, &peers[i]);
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
		if (filter_ == nullptr) return;
		for (const Ports::Bank& bank : ports_.banks()) {
			const BankSpan at{bank.first, bank.ports.size()};
			const Latency internal = internalOf(bank.name);
			spa_latency_info told{};
			if (bank.direction == Direction::output) {
				const Latency beyond = frames(combined(outputPeers_, at, SPA_DIRECTION_INPUT));
				for (std::size_t i = at.first; i < at.first + at.count; ++i)
					downstream_[i].store(packed(beyond), std::memory_order_release);
				// Upstream of an output: what reaches the paired inputs, plus the run's own.
				spa_latency_info_combine_start(&told, SPA_DIRECTION_OUTPUT);
				for (const BankPair& pair : plan_.pairs) {
					if (pair.to != bank.name) continue;
					const spa_latency_info upstream = combined(inputPeers_, span(pair.from), SPA_DIRECTION_OUTPUT);
					spa_latency_info_combine(&told, &upstream);
				}
			} else {
				// Downstream of an input: what lies beyond the paired outputs, plus the run's own.
				spa_latency_info_combine_start(&told, SPA_DIRECTION_INPUT);
				for (const BankPair& pair : plan_.pairs) {
					if (pair.from != bank.name) continue;
					const spa_latency_info downstream = combined(outputPeers_, span(pair.to), SPA_DIRECTION_INPUT);
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

	/// Data loop: drive()'s barrier.
	static int onDrive(spa_loop* /*loop*/, bool /*async*/, std::uint32_t /*seq*/, const void* data,
	                   std::size_t /*size*/, void* userData) {
		auto* self = static_cast<FilterPortBlock*>(userData);
		self->code_.store(*static_cast<AudioCode* const*>(data), std::memory_order_release);
		return 0;
	}

	/// Realtime thread: run the code (or silence), meter the public ports, and publish the
	/// window when this cycle crosses a window boundary of the graph clock.
	static void onProcess(void* data, spa_io_position* position) {
		auto* self = static_cast<FilterPortBlock*>(data);
		const spa_io_clock& clock = position->clock;
		const auto frames = static_cast<std::uint32_t>(clock.duration);
		// The clock moved on by the last cycle's frames on the same driver, or it did not: a new driver, a resync.
		const bool sameDriver = self->cycles_ > 0 && clock.id == self->lastDriver_;
		const bool discontinuity = !sameDriver || clock.position != self->nextPosition_;
		if (sameDriver && clock.position > self->nextPosition_) ++self->xruns_;
		++self->cycles_;
		self->lastDriver_ = clock.id;
		self->nextPosition_ = clock.position + clock.duration;
		if (frames <= self->maxBlock_) {
			// The driver's clock.nsec is CLOCK_MONOTONIC even while it tracks the raw clock (docs/design/node-framework.md
			// section 10); both clocks are read together here, so a late wake cancels out.
			const std::int64_t raw = nowNs(CLOCK_MONOTONIC_RAW);
			const std::int64_t mono = nowNs(CLOCK_MONOTONIC);
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
	// Latency: each output's downstream, published by the main thread for the realtime side (packed), and the copy
	// a cycle hands its code; then, on the main thread only, what each port's peers report, what each port told them
	// last, and the run's plan.
	std::vector<std::atomic<std::uint64_t>> downstream_;
	std::vector<Latency> downstreamNow_;
	std::vector<spa_latency_info> inputPeers_;  // upstream of each input
	std::vector<spa_latency_info> outputPeers_; // downstream of each output
	std::vector<spa_latency_info> inputTold_;
	std::vector<spa_latency_info> outputTold_;
	LatencyPlan plan_;
	pw_filter* filter_ = nullptr;
	spa_hook listener_{};
	bool readyTold_ = false;
	bool lostTold_ = false;
	spa_source* refusal_ = nullptr;
	std::string refusalDetail_;
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
