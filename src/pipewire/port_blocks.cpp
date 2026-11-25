// Port blocks as pw_filter nodes (docs/engine.md, port_blocks.hpp): every block in EAR's graph group, an asynchronous
// drive applied on the block's data loop, and level metering of the public ports in windows aligned on the graph clock.
// Each cycle's GraphCycle is made here, both clock domains sampled once (docs/engine.md#graphcycle), and latency is
// carried per bank pair under PW_FILTER_FLAG_CUSTOM_LATENCY. The asynchronous drive and fade
// (docs/engine.md#drive-and-fade): requests in a ring of eight, one invoke in flight per block, applied inside it,
// completed through the block's own event source. The ring companions ride in each drive, bound with the code in its
// invoke: each source (docs/engine.md#sources) is played into the block's own scratch for its inside input bank before
// the code, told the downstream latency the block last told on that bank, and each sink's tap (docs/engine.md#sinks) is
// written after the code and the fade, stamped with the upstream latency the block last told on its bank; both every
// cycle that code runs. The fade's ramp and both stamps are core's block rules (ear/core/block_rules.hpp), and each
// cycle's facts core's CycleAccount over the engine's record of the internal clock's cuts (ear/core/cycle_facts.hpp,
// docs/engine.md#graphcycle), which the type harness's block runs too. Everything the realtime side reads that is
// sized by the ports is one Layout, built whole on the main loop and installed by one pointer in a request's invoke, so
// a reshape (docs/engine.md#reshapes) resizes nothing the data loop may be reading.
#include "ear/pipewire/port_blocks.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
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
#include "ear/core/cycle_counters.hpp"
#include "ear/core/cycle_facts.hpp"
#include "ear/core/internal_error.hpp"
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
	/// Guards `blocks` and every mailbox's read side. Taken on the main loop (make, destroy, a reshape's layout
	/// installed) and by latestLevels' caller, never on the realtime thread.
	std::mutex mutex;
	std::vector<RegisteredBlock*> blocks;
};

/// A block's entry in its factory's levels registry, held by the block: added once, moved to a reshape's new layout's
/// mailbox once that layout is installed, erased when it goes (or when the block's constructor throws after adding it),
/// so latestLevels never reads a freed block's mailbox. Counted with the boundary's registrations, so the injection seam
/// covers it (docs/testing.md#the-tools).
class LevelsEntry {
public:
	LevelsEntry() = default;
	LevelsEntry(const LevelsEntry&) = delete;
	LevelsEntry& operator=(const LevelsEntry&) = delete;
	~LevelsEntry() { reset(); }
	void add(std::shared_ptr<Levels> levels, RegisteredBlock& block) {
		reset();
		{
			const std::scoped_lock lock(levels->mutex);
			levels->blocks.push_back(&block);
		}
		levels_ = std::move(levels);
		block_ = &block;
		boundary::held();
	}
	/// The entry now reads `block` (a reshape's layout, installed); the one it read may be freed on return.
	void move(RegisteredBlock& block) {
		if (block_ == nullptr || block_ == &block) return;
		const std::scoped_lock lock(levels_->mutex);
		std::ranges::replace(levels_->blocks, block_, &block);
		block_ = &block;
	}
	void reset() noexcept {
		if (block_ == nullptr) return;
		{
			const std::scoped_lock lock(levels_->mutex);
			std::erase(levels_->blocks, block_);
		}
		block_ = nullptr;
		boundary::released();
	}

private:
	std::shared_ptr<Levels> levels_;
	RegisteredBlock* block_ = nullptr;
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

/// How many inside banks of a direction the ports have: the most sinks (output) or sources (input) a drive binds.
std::size_t insideBanks(const Ports& ports, Direction direction) {
	return static_cast<std::size_t>(std::ranges::count_if(ports.banks(), [direction](const Ports::Bank& bank) {
		return bank.reach == PortBank::Reach::inside && bank.direction == direction;
	}));
}

/// A port of the block in the engine: its direction and its whole name (a public and an inside port never share one).
using PortKey = std::pair<Direction, PortName>;

/// Every port of `ports` in process() order: inputs then inside inputs, outputs then inside outputs.
std::vector<PortKey> processOrder(const Ports& ports, Direction direction) {
	std::vector<PortKey> keys;
	const auto add = [&](const std::vector<PortName>& names) {
		for (const PortName& name : names) keys.emplace_back(direction, name);
	};
	if (direction == Direction::input) {
		add(ports.inputs());
		add(ports.insideInputs());
	} else {
		add(ports.outputs());
		add(ports.insideOutputs());
	}
	return keys;
}

/// Where each of `now`'s keys sat in `was` (-1: not there).
std::vector<std::int32_t> indexMap(const std::vector<PortKey>& now, const std::vector<PortKey>& was) {
	std::vector<std::int32_t> map(now.size(), -1);
	for (std::size_t i = 0; i < now.size(); ++i) {
		const auto found = std::ranges::find(was, now[i]);
		if (found != was.end()) map[i] = static_cast<std::int32_t>(found - was.begin());
	}
	return map;
}

/// Everything the realtime side reads that the ports size (docs/engine.md#reshapes), for one view: the ports a code
/// was laid out for. Built whole on the main loop, every allocation there, and installed by one pointer in a request's
/// invoke; nothing in it is resized after. `zeroFill` lists the outputs the block's ports have beyond the view (a
/// reshape's new outputs while the old code runs): each cycle writes them zeros. The `from` maps say where each port sat
/// in the layout installed before it, so the meters, the holds and the fade mask carry on across the install.
struct Layout {
	Layout(const Ports& ports, std::vector<void*> inputHandles, std::vector<void*> outputHandles,
	       std::vector<void*> zeros, const NodeName& node, const AudioFormat& format)
		: view(ports), inputPorts(std::move(inputHandles)), outputPorts(std::move(outputHandles)),
		  zeroFill(std::move(zeros)), publicInputs(ports.inputs().size()), publicOutputs(ports.outputs().size()),
		  inputs(inputPorts.size()), outputs(outputPorts.size()), meters(publicInputs + publicOutputs),
		  registered(node, publicPorts(ports)), downstream(outputPorts.size()), downstreamNow(outputPorts.size()),
		  inputToldNow(inputPorts.size()), outputToldNow(outputPorts.size()), hold(format.rate, outputPorts.size()),
		  fadeMask(outputPorts.size(), 0), sinks(insideBanks(ports, Direction::output)), tapSpans(outputPorts.size()),
		  sources(insideBanks(ports, Direction::input)), sourceSpans(inputPorts.size()) {
		for (std::size_t i = 0; i < outputPorts.size(); ++i) scratch.emplace_back(format.maxBlock, 0.0f);
		// A source plays into scratch of the block's own, never into PipeWire's buffer of its (unlinked) port: one
		// buffer per inside input port.
		for (std::size_t i = publicInputs; i < inputPorts.size(); ++i)
			sourceScratch.emplace_back(format.maxBlock, 0.0f);
	}
	Layout(const Layout&) = delete;
	Layout& operator=(const Layout&) = delete;

	Ports view;
	std::vector<void*> inputPorts; // the view's handles, in process() order (null: a port the engine refused)
	std::vector<void*> outputPorts;
	std::vector<void*> zeroFill;
	std::size_t publicInputs;
	std::size_t publicOutputs;
	std::vector<std::span<const float>> inputs;
	std::vector<std::span<float>> outputs;
	std::vector<std::vector<float>> scratch;       // an output with no buffer this cycle writes here
	std::vector<std::vector<float>> sourceScratch; // per inside input port: what a source plays
	std::vector<LevelMeter> meters;                // public inputs, then public outputs
	RegisteredBlock registered;
	// Latency, published by the main thread for the realtime side (packed): each output's downstream, and what each port
	// last told its peers (an input's playback latency, an output's capture latency); the copy a cycle hands its code.
	std::vector<std::atomic<std::uint64_t>> downstream;
	std::vector<Latency> downstreamNow;
	std::vector<std::atomic<std::uint64_t>> inputToldNow;
	std::vector<std::atomic<std::uint64_t>> outputToldNow;
	HoldRamp hold;
	std::vector<std::uint8_t> fadeMask;           // per output
	std::vector<SinkBinding> sinks;               // the applied drive's taps, sized for the view's inside banks
	std::size_t sinkCount = 0;                    // how many of sinks are bound
	std::vector<std::span<const float>> tapSpans; // one tap's bank, built per cycle
	std::vector<SourceBinding> sources;
	std::size_t sourceCount = 0;
	std::vector<std::span<float>> sourceSpans;
	// Where each output and each metered port sat in the layout installed before this one (-1: new): main thread, set
	// when the layout is asked for, read in the invoke that installs it.
	std::vector<std::int32_t> outputFrom;
	std::vector<std::int32_t> meterFrom;
};

/// A block's state is its node's alone, so a throw out of its own callback fails only that node
/// (docs/architecture.md#faults-by-owner).
class FilterPortBlock final : public PortBlock, public FaultOwner {
public:
	FilterPortBlock(Engine& engine, std::shared_ptr<Levels> levels, const NodeName& node, const Ports& ports,
	                const AudioFormat& format, PortBlockEvents& events)
		: engine_(engine), events_(events), loop_(engine.mainLoop()), node_(node), format_(format),
		  maxBlock_(format.maxBlock), windowFrames_(std::max(levelWindowFrames(format.rate), 1U)),
		  zeros_(format.maxBlock, 0.0f), levels_(std::move(levels)), target_(ports), latencyPorts_(ports),
		  rate_(format.rate), cuts_(engine.cuts()), internalClock_(engine.internalClockId()),
		  account_(FrameClock(format.rate), &cuts_), cycleClock_(format.rate), fade_(format.rate) {
		// Each registration is held by a member declared, like it, in this order, so a throw anywhere below unwinds
		// every one made so far before anything it calls into goes (docs/architecture.md#faults-by-owner).
		addEvent(refusal_, loop_, guarded<&FilterPortBlock::onRefusal>, this);
		addEvent(completion_, loop_, guarded<&FilterPortBlock::onCompletion>, this);
		// Every key from the property table (docs/engine.md#properties-ear-sets says why each).
		filter_.reset(pw_filter_new(engine.core(), node.c_str(),
		                            PropertySet<Home::block>({{property<Home::block>("node.name"), node},
		                                                      {property<Home::block>("media.name"), node},
		                                                      {property<Home::block>("media.type"), "Audio"},
		                                                      {property<Home::block>("node.group"), graphGroup},
		                                                      {property<Home::block>("node.always-process"), "true"}})
		                                .release()));
		if (filter_ == nullptr) fail(std::string("cannot make the engine node: ") + spa_strerror(-errno));
		else
			listen<FilterCallbacks<FilterPortBlock>{.stateChanged = guarded<&FilterPortBlock::onStateChanged>,
			                                        .paramChanged = guarded<&FilterPortBlock::onParamChanged>,
			                                        .process = realtime<&FilterPortBlock::onProcess>}>(filter_.get(),
			                                                                                           listener_, this);
		// AudioCode::process sees inputs then inside inputs, outputs then inside outputs.
		Layout& first = layoutFor(ports);
		rt_ = &first;
		installed_ = &first;
		lastAsked_ = &first;
		levelsEntry_.add(levels_, first.registered);
		if (filter_ == nullptr) return;
		// Custom latency: pw_filter would otherwise merge every output's downstream latency into every input (and
		// every input's upstream into every output), mixing paths that never meet (filter.c, default_latency). The
		// block carries it per bank pair instead (carryLatency).
		if (const int res = pw_filter_connect(
				filter_.get(), static_cast<pw_filter_flags>(PW_FILTER_FLAG_RT_PROCESS | PW_FILTER_FLAG_CUSTOM_LATENCY),
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
		listener_.remove();
		filter_.reset();
		if (postedLoop_ && invokesPosted_ != invokesRun_.load(std::memory_order_acquire)) {
			invoke(postedLoop_, realtime<&FilterPortBlock::onNothing>, this, true);
		}
		completion_.reset();
		refusal_.reset();
		levelsEntry_.reset();
	}

	FilterPortBlock(const FilterPortBlock&) = delete;
	FilterPortBlock& operator=(const FilterPortBlock&) = delete;

	void drive(DriveRequest request, std::move_only_function<void(std::uint64_t)> done) override {
		// A sink reads an inside output bank, a source plays into an inside input bank, and NodeHost gives each bank at
		// most one companion, so a drive binds at most one tap per such bank: each layout's realtime storage is sized for
		// that, and apply() binds no more than it holds. Nothing is thrown here: this runs on the main loop, inside
		// PipeWire's C callbacks.
		Layout* layout = layoutForCode(request);
		pending_.push_back(Pending{
			Kind::drive, request.code, request.faded, request.sinks, request.sources, std::move(done), {}, {}, layout});
		pump();
	}

	void fadeOut(std::move_only_function<void()> done) override {
		pending_.push_back(Pending{Kind::fade, nullptr, {}, {}, {}, {}, std::move(done), {}, nullptr});
		pump();
	}

	void hold(std::vector<std::uint8_t> outputs, std::move_only_function<void()> acknowledged) override {
		holdAcks_.push_back(std::move(acknowledged));
		pending_.push_back(Pending{Kind::hold, nullptr, {}, {}, {}, {}, {}, std::move(outputs), nullptr});
		pump();
	}

	void release(std::vector<std::uint8_t> outputs) override {
		pending_.push_back(Pending{Kind::release, nullptr, {}, {}, {}, {}, {}, std::move(outputs), nullptr});
		pump();
	}

	void swap(DriveRequest request, std::move_only_function<void(std::uint64_t)> done) override {
		Layout* layout = layoutForCode(request);
		pending_.push_back(Pending{
			Kind::swap, request.code, request.faded, request.sinks, request.sources, std::move(done), {}, {}, layout});
		pump();
	}

	/// docs/engine.md#reshapes: the ports `next` adds go into the engine now; the code last asked keeps its view, with
	/// the new outputs written zeros, unless it is a silence, which takes `next`'s view at once. The ports `next` drops
	/// go once the layout installed is `next`'s and no request waits with another (collect()).
	void reshape(const Ports& next) override {
		if (next == target_) return;
		target_ = next;
		Layout& layout = layoutFor(lastCode_ != nullptr ? lastAsked_->view : target_);
		pending_.push_back(Pending{Kind::layout, nullptr, {}, {}, {}, {}, {}, {}, &layout});
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
			if (next.layout != nullptr) installedLayout(*next.layout);
			if (next.kind == Kind::drive || next.kind == Kind::swap) {
				if (next.driven) next.driven(taken);
			} else if (next.kind == Kind::fade && !cancelled && next.faded) {
				next.faded();
			}
			if (alive.expired()) return;
		}
		// Every hold the data loop acknowledged, in the order asked.
		const std::uint64_t acknowledged = holdsAcked_.load(std::memory_order_acquire);
		while (holdAcksRun_ < acknowledged && !holdAcks_.empty()) {
			std::move_only_function<void()> heard = std::move(holdAcks_.front());
			holdAcks_.pop_front();
			++holdAcksRun_;
			if (heard) heard();
			if (alive.expired()) return;
		}
		pump();
	}

	std::uint64_t takenAt() const noexcept override { return takenAt_.load(std::memory_order_acquire); }

	std::uint32_t blockId() const noexcept override { return blockId_; }

	NodeCounts counts() const noexcept override { return counters_.read(); }

	void carryLatency(const LatencyPlan& plan) override {
		// Worked out here first, so a plan that does not fit the ports throws to its caller and is never kept: after
		// this only the peers change, so no later retell, inside a peer's param_changed, meets a bad plan. A plan is for
		// the block's ports, or, while a reshape waits for its code, for the view of the code last asked.
		LatencyFacts facts;
		const Ports* ports = &target_;
		try {
			facts = workOut(target_, plan, peers(target_, Direction::input), peers(target_, Direction::output));
		} catch (const std::logic_error&) {
			if (lastAsked_->view == target_) throw;
			ports = &lastAsked_->view;
			facts = workOut(*ports, plan, peers(*ports, Direction::input), peers(*ports, Direction::output));
		}
		plan_ = plan;
		latencyPorts_ = *ports;
		tell(facts);
	}

private:
	/// The engine handle of a port the block's ports name, added to the engine first if it is not there
	/// (docs/engine.md#reshapes); null for a port the engine refused, a block with no filter or a wedged data loop.
	void* handle(const PortKey& key) {
		if (const auto found = engine_ports_.find(key); found != engine_ports_.end()) return found->second;
		void* added = nullptr;
		// Adding a port to a connected filter reaches the data loop (impl-port.c), so none is added once it is wedged.
		if (filter_ != nullptr && !dataLoopWedged(engine_)) {
			// The handle is the port's (empty) user data, freed with the port; a refused port (null) leaves its span on
			// zeros or scratch.
			added = pw_filter_add_port(
				filter_.get(), key.first == Direction::input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
				PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
				PropertySet<Home::blockPort>({{property<Home::blockPort>("format.dsp"), "32 bit float mono audio"},
			                                  {property<Home::blockPort>("port.name"), key.second}})
					.release(),
				nullptr, 0);
		}
		engine_ports_.emplace(key, added);
		return added;
	}

	/// A layout for `view` (main thread), its ports in the engine and every port of the block's own ports too, kept
	/// until a later one is installed. Its `from` maps are against the layout the data loop will hold before it: the
	/// last asked.
	Layout& layoutFor(const Ports& view) {
		const std::vector<PortKey> inputs = processOrder(view, Direction::input);
		const std::vector<PortKey> outputs = processOrder(view, Direction::output);
		std::vector<void*> inputHandles;
		std::vector<void*> outputHandles;
		for (const PortKey& key : inputs) inputHandles.push_back(handle(key));
		for (const PortKey& key : outputs) outputHandles.push_back(handle(key));
		std::vector<void*> zeroFill;
		for (const PortKey& key : processOrder(target_, Direction::input)) static_cast<void>(handle(key));
		for (const PortKey& key : processOrder(target_, Direction::output))
			if (std::ranges::find(outputs, key) == outputs.end())
				if (void* port = handle(key)) zeroFill.push_back(port);
		auto& layout = *layouts_.emplace_back(std::make_unique<Layout>(
			view, std::move(inputHandles), std::move(outputHandles), std::move(zeroFill), node_, format_));
		if (lastAsked_ != nullptr) {
			layout.outputFrom = indexMap(outputs, processOrder(lastAsked_->view, Direction::output));
			const auto metered = [](const Ports& ports) {
				std::vector<PortKey> keys;
				for (const PortName& name : ports.inputs()) keys.emplace_back(Direction::input, name);
				for (const PortName& name : ports.outputs()) keys.emplace_back(Direction::output, name);
				return keys;
			};
			layout.meterFrom = indexMap(metered(view), metered(lastAsked_->view));
		}
		lastAsked_ = &layout;
		publishLatency(layout);
		return layout;
	}

	/// The layout a drive or swap installs with its code: none when the data loop will already hold its view.
	Layout* layoutForCode(const DriveRequest& request) {
		lastCode_ = request.code;
		const Ports& view = request.ports != nullptr ? *request.ports : target_;
		if (view == lastAsked_->view) return nullptr;
		return &layoutFor(view);
	}

	/// Main thread, once the request that installs `layout` completed: the data loop reads it and no layout before it,
	/// so the levels read its mailbox, the earlier layouts are freed, and the ports no layout left names may go.
	void installedLayout(Layout& layout) {
		installed_ = &layout;
		levelsEntry_.move(layout.registered);
		while (!layouts_.empty() && layouts_.front().get() != &layout) layouts_.pop_front();
		collect();
	}

	/// docs/engine.md#reshapes: once the layout installed is the last asked and holds the block's own ports, every
	/// engine port they do not name is removed: no layout the data loop may read holds its handle, so the port's memory
	/// may go with pw_filter_remove_port. Removing a port waits on the data loop (impl-port.c), so on a wedged one the
	/// ports are left.
	void collect() {
		if (installed_ != lastAsked_ || installed_->view != target_) return;
		const std::vector<PortKey> inputs = processOrder(target_, Direction::input);
		const std::vector<PortKey> outputs = processOrder(target_, Direction::output);
		for (auto it = engine_ports_.begin(); it != engine_ports_.end();) {
			const std::vector<PortKey>& kept = it->first.first == Direction::input ? inputs : outputs;
			if (std::ranges::find(kept, it->first) != kept.end()) {
				++it;
				continue;
			}
			if (it->second != nullptr) {
				if (filter_ == nullptr || dataLoopWedged(engine_)) return;
				peers_.erase(it->second);
				told_.erase(it->second);
				pw_filter_remove_port(it->second);
			}
			it = engine_ports_.erase(it);
		}
	}

	/// What each of `ports`' ports' peers last reported, in process() order (none: no peer reported).
	std::vector<std::optional<Latency>> peers(const Ports& ports, Direction direction) const {
		std::vector<std::optional<Latency>> heard;
		for (const PortKey& key : processOrder(ports, direction)) {
			const auto port = engine_ports_.find(key);
			const auto peer = port == engine_ports_.end() ? peers_.end() : peers_.find(port->second);
			heard.push_back(peer == peers_.end() ? std::nullopt : peer->second);
		}
		return heard;
	}

	/// A callback of this block's threw (its bug): it is lost for good, and its node fails lost.
	std::string_view faultNode() const noexcept override { return node_; }
	/// The block leaves the graph, so nothing it half-changed runs on the data loop again: its listener now (the
	/// process callback with it; removing a hook inside its own event is allowed), and, later on the main loop, outside
	/// any of the filter's events, the filter and any invoke of its still queued, as its destructor does it. From
	/// then on every request is applied on the main thread and completed by signal, so the silences its node's
	/// teardown asks for complete and no bound takes the data loop for wedged (docs/architecture.md#faults-by-owner).
	/// On a wedged data loop each step that would wait on it is skipped by the boundary, which leaks instead
	/// (boundary::dataLoopWedged), and nothing is applied on the main thread (leave()).
	void faulted(std::string_view callback, std::string_view detail) override {
		listener_.remove();
		processing_.store(false, std::memory_order_release);
		if (!lostTold_) {
			lostTold_ = true;
			events_.faulted(internalErrorDetail(callback, detail));
		}
		leaving_ = true;
		if (refusal_) pw_loop_signal_event(loop_, refusal_.get());
	}

	/// Main loop, outside the filter's events: the rest of faulted()'s leaving. On a wedged data loop the filter's
	/// process stays installed (its listener leaked), so nothing is applied here either, where it would race that
	/// loop's apply should it ever run again.
	void leave() {
		if (dataLoopWedged(engine_)) return;
		filter_.reset();
		if (postedLoop_ && invokesPosted_ != invokesRun_.load(std::memory_order_acquire))
			invoke(postedLoop_, realtime<&FilterPortBlock::onNothing>, this, true);
		postedLoop_ = DataLoop();
		kick_ = true;
		pump();
	}

	/// A refusal while making the block is told on the main loop, never inside make().
	void fail(std::string detail) {
		refusalDetail_ = std::move(detail);
		if (refusal_) pw_loop_signal_event(loop_, refusal_.get());
	}

	void onRefusal(std::uint64_t /*count*/) {
		if (std::exchange(leaving_, false)) leave();
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
			blockId_ = pw_filter_get_node_id(filter_.get());
		}
		if (!streaming && old == PW_FILTER_STATE_STREAMING &&
		    (completed_.load(std::memory_order_acquire) < pushed_ || !holdAcks_.empty())) {
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
		const auto known =
			std::ranges::find_if(engine_ports_, [port](const auto& entry) { return entry.second == port; });
		if (known == engine_ports_.end()) return;
		const spa_direction farSide =
			known->first.first == Direction::input ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
		spa_latency_info info{};
		if (param == nullptr) peers_[port] = std::nullopt;
		else if (spa_latency_parse(param, &info) >= 0 && info.direction == farSide)
			peers_[port] = fromSpa(info, maxBlock_, FrameClock(rate_));
		tell(workOut(latencyPorts_, plan_, peers(latencyPorts_, Direction::input),
		             peers(latencyPorts_, Direction::output)));
	}

	/// Main thread: publishes what the model worked out (docs/engine.md#latency-per-bank-pair) to the realtime side of
	/// every layout kept, each output's downstream and each port's told latency, and tells each port's new told latency
	/// to its peers, in rate units.
	void tell(const LatencyFacts& facts) {
		facts_ = facts;
		for (const auto& layout : layouts_) publishLatency(*layout);
		// On a wedged data loop no latency is told to the peers (data_loop_wedge.hpp): nothing more reaches the
		// engine. What the realtime side reads is still published: it is only atomics.
		if (filter_ == nullptr || dataLoopWedged(engine_)) return;
		const auto tellPeers = [&](Direction direction, std::size_t base, spa_direction side) {
			const std::vector<PortKey> keys = processOrder(latencyPorts_, direction);
			for (std::size_t i = 0; i < keys.size() && base + i < facts.told.size(); ++i) {
				const auto port = engine_ports_.find(keys[i]);
				if (port == engine_ports_.end() || port->second == nullptr) continue;
				const Latency told = facts.told[base + i];
				const auto last = told_.find(port->second);
				if (last != told_.end() ? last->second == told : told == Latency{}) continue;
				told_[port->second] = told;
				const spa_latency_info info = toSpa(told, side);
				std::uint8_t buffer[256];
				spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
				const spa_pod* params[1] = {
					static_cast<const spa_pod*>(spa_latency_build(&builder, SPA_PARAM_Latency, &info))};
				pw_filter_update_params(filter_.get(), port->second, params, 1);
			}
		};
		tellPeers(Direction::input, 0, SPA_DIRECTION_INPUT);
		tellPeers(Direction::output, processOrder(latencyPorts_, Direction::input).size(), SPA_DIRECTION_OUTPUT);
	}

	/// Main thread: the latest facts into one layout's atomics, by port: an output's capture latency stamps its sink's
	/// chunks, an input's playback latency dates what its source played; a port the facts' ports do not name reads 0.
	void publishLatency(Layout& layout) const {
		const std::vector<PortKey> latencyInputs = processOrder(latencyPorts_, Direction::input);
		const std::vector<PortKey> latencyOutputs = processOrder(latencyPorts_, Direction::output);
		const auto at = [](const std::vector<PortKey>& keys, const PortKey& key) -> std::optional<std::size_t> {
			const auto found = std::ranges::find(keys, key);
			if (found == keys.end()) return std::nullopt;
			return static_cast<std::size_t>(found - keys.begin());
		};
		const std::vector<PortKey> inputs = processOrder(layout.view, Direction::input);
		const std::vector<PortKey> outputs = processOrder(layout.view, Direction::output);
		for (std::size_t i = 0; i < inputs.size(); ++i) {
			const auto index = at(latencyInputs, inputs[i]);
			const Latency told = index && *index < facts_.told.size() ? facts_.told[*index] : Latency{};
			layout.inputToldNow[i].store(packed(told), std::memory_order_release);
		}
		for (std::size_t i = 0; i < outputs.size(); ++i) {
			const auto index = at(latencyOutputs, outputs[i]);
			const std::size_t told = index ? latencyInputs.size() + *index : facts_.told.size();
			layout.outputToldNow[i].store(packed(told < facts_.told.size() ? facts_.told[told] : Latency{}),
			                              std::memory_order_release);
			const Latency down = index && *index < facts_.downstream.size() ? facts_.downstream[*index] : Latency{};
			layout.downstream[i].store(packed(down), std::memory_order_release);
		}
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
			                                 .outputs = next.outputs.data(),
			                                 .outputCount = next.outputs.size(),
			                                 .sinks = next.sinks.data(),
			                                 .sinkCount = next.sinks.size(),
			                                 .sources = next.sources.data(),
			                                 .sourceCount = next.sources.size(),
			                                 .layout = next.layout};
			++pushed_;
		}
		published_.store(pushed_, std::memory_order_release);
		if (invokesPosted_ != invokesRun_.load(std::memory_order_acquire)) return; // its completion pumps again
		if (drained_.load(std::memory_order_acquire) == pushed_ && !kick_) return;
		kick_ = false;
		const DataLoop dataLoop = filter_ != nullptr ? DataLoop::of(filter_.get()) : DataLoop();
		if (!dataLoop) {
			// Nothing processes: applied here, and still completed by signal.
			apply();
			pw_loop_signal_event(loop_, completion_.get());
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
		pw_loop_signal_event(loop_, completion_.get());
	}

	/// The data loop: nothing, so a blocking invoke of it flushes the invokes posted before it.
	void onNothing() noexcept {}

	/// Data loop: `next` becomes the layout every cycle reads, carrying from the one it replaces what goes on by port
	/// (the meters' window, each output's hold and fade flag) and the bound companions (a layout a reshape installs
	/// keeps its code's view, so their indices hold; a drive's or swap's binds its own after this).
	void install(Layout& next) noexcept {
		Layout& old = *rt_;
		next.hold.carry(old.hold, next.outputFrom);
		for (std::size_t o = 0; o < next.fadeMask.size(); ++o) {
			const std::int32_t from = o < next.outputFrom.size() ? next.outputFrom[o] : -1;
			next.fadeMask[o] = from >= 0 && static_cast<std::size_t>(from) < old.fadeMask.size()
			                       ? old.fadeMask[static_cast<std::size_t>(from)]
			                       : std::uint8_t{0};
		}
		for (std::size_t m = 0; m < next.meters.size(); ++m) {
			const std::int32_t from = m < next.meterFrom.size() ? next.meterFrom[m] : -1;
			if (from >= 0 && static_cast<std::size_t>(from) < old.meters.size())
				next.meters[m] = old.meters[static_cast<std::size_t>(from)];
		}
		next.sinkCount = std::min(old.sinkCount, next.sinks.size());
		std::copy_n(old.sinks.begin(), next.sinkCount, next.sinks.begin());
		next.sourceCount = std::min(old.sourceCount, next.sources.size());
		std::copy_n(old.sources.begin(), next.sourceCount, next.sources.begin());
		rt_ = &next;
	}

	/// Data loop (or the main thread of a block with no data loop): drains the ring.
	void apply() noexcept {
		const std::uint64_t until = published_.load(std::memory_order_acquire);
		std::uint64_t drained = drained_.load(std::memory_order_relaxed);
		for (; drained < until; ++drained) {
			Request& request = ring_[drained % kRing];
			if (request.layout != nullptr) install(*request.layout);
			Layout& layout = *rt_;
			if (request.kind == Kind::layout) continue;
			if (request.kind == Kind::fade) {
				request.cancelled = false;
				fade_.fadeOut();
				continue;
			}
			if (request.kind == Kind::hold || request.kind == Kind::release) {
				const std::span<const std::uint8_t> marked(request.outputs, request.outputCount);
				if (request.kind == Kind::hold) layout.hold.hold(marked);
				else layout.hold.release(marked);
				continue;
			}
			code_.store(request.code, std::memory_order_release);
			// The taps change with the code, in this invoke: copied into storage sized at the layout's making.
			layout.sinkCount = std::min(request.sinkCount, layout.sinks.size());
			std::copy_n(request.sinks, layout.sinkCount, layout.sinks.begin());
			layout.sourceCount = std::min(request.sourceCount, layout.sources.size());
			std::copy_n(request.sources, layout.sourceCount, layout.sources.begin());
			account_.driven(); // the new code's first cycle is a discontinuity to it
			// A swap keeps the fade and the holds: the new code takes the next cycle at the gains the old one had.
			if (request.kind == Kind::drive) {
				bool anyFaded = false;
				for (std::size_t i = 0; i < layout.fadeMask.size(); ++i) {
					layout.fadeMask[i] = i < request.fadedCount && request.faded[i] ? 1 : 0;
					anyFaded = anyFaded || layout.fadeMask[i] != 0;
				}
				// The fade-outs waiting, the newest requests before this one, are cancelled (BlockFade's rule).
				const std::uint64_t cancelled = fade_.driven(anyFaded);
				for (std::uint64_t pending = drained - cancelled; pending < drained; ++pending)
					ring_[pending % kRing].cancelled = true;
				holdsAckedRt_ += layout.hold.driven();
			} else if (request.layout != nullptr) {
				// A swap to another layout: its outputs fade as its own code's mask says (the fade is at full gain,
				// since a swap is asked only while no fade-out waits).
				for (std::size_t i = 0; i < layout.fadeMask.size(); ++i)
					layout.fadeMask[i] = i < request.fadedCount && request.faded[i] ? 1 : 0;
			}
			request.takenAt = account_.nextPosition();
			takenAt_.store(account_.nextPosition(), std::memory_order_release);
		}
		drained_.store(drained, std::memory_order_release);
		if (!processing_.load(std::memory_order_acquire)) { // no cycle will come to end them
			fade_.notProcessing();
			holdsAckedRt_ += rt_->hold.notProcessing();
		}
		holdsAcked_.store(holdsAckedRt_, std::memory_order_release);
		// Requests complete in order and only fade-outs wait, so every request before the waiting ones is complete.
		completed_.store(drained - fade_.waiting(), std::memory_order_release);
	}

	/// Realtime thread, after the code: the fade on the faded outputs (BlockFade), and the fade-outs waiting
	/// acknowledged at the end of the first cycle that ended at zero.
	void applyFade(Layout& layout, std::uint32_t frames) noexcept {
		if (fade_.apply(layout.outputs, layout.fadeMask, frames).acknowledged > 0) acknowledgeFade();
	}

	/// Realtime thread, after the fade: the holds (HoldRamp), and those waiting acknowledged at the end of the first
	/// cycle that ended with every held output at zero.
	void applyHolds(Layout& layout, std::uint32_t frames) noexcept {
		acknowledgeHolds(layout.hold.apply(layout.outputs, frames));
	}

	/// Realtime thread: `count` more holds are acknowledged.
	void acknowledgeHolds(std::uint64_t count) noexcept {
		if (count == 0) return;
		holdsAckedRt_ += count;
		holdsAcked_.store(holdsAckedRt_, std::memory_order_release);
		pw_loop_signal_event(loop_, completion_.get());
	}

	/// Realtime thread: every pending fade-out is done.
	void acknowledgeFade() noexcept {
		const std::uint64_t drained = drained_.load(std::memory_order_relaxed);
		if (completed_.load(std::memory_order_relaxed) == drained) return;
		completed_.store(drained, std::memory_order_release);
		pw_loop_signal_event(loop_, completion_.get());
	}

	/// Main loop: the data loop completed requests, or an invoke ran.
	void onCompletion(std::uint64_t /*count*/) { settle(); }

	/// Realtime thread: run the code (or silence), meter the public ports, and publish the
	/// window when this cycle crosses a window boundary of the graph clock.
	void onProcess(spa_io_position* position) noexcept {
		Layout& layout = *rt_;
		const spa_io_clock& clock = position->clock;
		const auto frames = static_cast<std::uint32_t>(clock.duration);
		// The driver's clock.nsec is CLOCK_MONOTONIC even while it tracks the raw clock
		// (docs/engine.md#the-internal-clock); both clocks are read together here, so a late wake cancels out. The raw
		// read sits between two monotonic ones, and the offset is taken from their midpoint, so a preemption between
		// reads skews it by half at most (node-driver.c, get_nsec_offset).
		const std::int64_t monoBefore = nowNs(CLOCK_MONOTONIC);
		const std::int64_t raw = nowNs(CLOCK_MONOTONIC_RAW);
		const std::int64_t monoAfter = nowNs(CLOCK_MONOTONIC);
		const std::int64_t mono = monoBefore + (monoAfter - monoBefore) / 2;
		const auto startNs = static_cast<std::int64_t>(clock.nsec);
		const std::int64_t startRawNs = startNs + (raw - mono);
		// The cycle names itself by the record's count, which the driver's start moved on just before it triggered the
		// followers, on this thread (docs/engine.md#graphcycle); a cycle of another driver has no number.
		const CycleAccount::Step step =
			account_.next(DriverTick{.driverId = clock.id,
		                             .graphCycle = clock.id == internalClock_ ? cuts_.started() : 0,
		                             .position = clock.position,
		                             .frames = frames,
		                             .startRawNs = startRawNs},
		                  maxBlock_);
		// The graph's record learns from the cycle's own overdue time when the graph is no longer behind, which ends a
		// stall of the host (docs/engine.md#counters).
		const std::int64_t overdueNs = cycleOverdueNs(mono, clock.next_nsec);
		if (clock.id == internalClock_ && overdueNs == 0) engine_.cycleOnTime();
		// A reshape's outputs beyond the code's view carry zeros until the code for them comes (docs/engine.md#reshapes).
		for (void* port : layout.zeroFill)
			if (auto* out = static_cast<float*>(pw_filter_get_dsp_buffer(port, frames))) std::fill_n(out, frames, 0.0f);
		if (step.runs) {
			for (std::size_t i = 0; i < layout.downstreamNow.size(); ++i)
				layout.downstreamNow[i] = unpacked(layout.downstream[i].load(std::memory_order_acquire));
			processCycle(layout, frames,
			             GraphCycle{.startNs = startNs,
			                        .position = clock.position,
			                        .startRawNs = startRawNs,
			                        .rateDiff = clock.rate_diff,
			                        .driverId = clock.id,
			                        .nextNs = static_cast<std::int64_t>(clock.next_nsec),
			                        .overdueNs = overdueNs,
			                        .facts = step.facts,
			                        .downstream = layout.downstreamNow});
		} else {
			for (void* port : layout.outputPorts) {
				if (port == nullptr) continue;
				if (auto* out = static_cast<float*>(pw_filter_get_dsp_buffer(port, frames))) {
					std::fill_n(out, frames, 0.0f);
				}
			}
			// It wrote zeros: a fade-out and the holds end here.
			if (fade_.wroteZeros() > 0) acknowledgeFade();
			acknowledgeHolds(layout.hold.wroteZeros());
		}
		// Every block of one driver sees the same clock position, so every block's windows end at
		// the same multiples of windowFrames_, whenever the block was made.
		const std::uint64_t start = position->clock.position;
		const std::uint64_t boundary = (start + frames) / windowFrames_;
		if (boundary != start / windowFrames_) publish(layout, boundary * windowFrames_);
		// The node's counters, after everything the block did in the cycle: its work timed from the monotonic reading
		// taken at entry, so a late wake is the host's, never the node's overrun (docs/engine.md#counters).
		counters_.add(step.facts, cycleOverran(nowNs(CLOCK_MONOTONIC) - monoBefore, cycleClock_, frames));
	}

	void processCycle(Layout& layout, std::uint32_t frames, const GraphCycle& cycle) noexcept {
		for (std::size_t i = 0; i < layout.inputPorts.size(); ++i) {
			const auto* in = layout.inputPorts[i] == nullptr
			                     ? nullptr
			                     : static_cast<const float*>(pw_filter_get_dsp_buffer(layout.inputPorts[i], frames));
			layout.inputs[i] = std::span<const float>(in != nullptr ? in : zeros_.data(), frames);
		}
		for (std::size_t i = 0; i < layout.outputPorts.size(); ++i) {
			auto* out = layout.outputPorts[i] == nullptr
			                ? nullptr
			                : static_cast<float*>(pw_filter_get_dsp_buffer(layout.outputPorts[i], frames));
			layout.outputs[i] = std::span<float>(out != nullptr ? out : layout.scratch[i].data(), frames);
		}
		// Each source, before the code, into the block's scratch for its inside input bank, which the code then reads
		// in place of the bank's ports; only while a code runs: a silence binds none.
		for (std::size_t s = 0; s < layout.sourceCount; ++s) {
			const SourceBinding& binding = layout.sources[s];
			if (binding.tap == nullptr || binding.channels == 0 || binding.firstInput < layout.publicInputs ||
			    binding.firstInput + binding.channels > layout.inputs.size())
				continue;
			for (std::size_t c = 0; c < binding.channels; ++c)
				layout.sourceSpans[c] =
					std::span<float>(layout.sourceScratch[binding.firstInput + c - layout.publicInputs].data(), frames);
			const Latency downstream =
				unpacked(layout.inputToldNow[binding.firstInput].load(std::memory_order_acquire)); // the bank's told
			binding.tap->play(std::span<const std::span<float>>(layout.sourceSpans.data(), binding.channels),
			                  sourceTick(cycle, frames, rate_, downstream));
			for (std::size_t c = 0; c < binding.channels; ++c)
				layout.inputs[binding.firstInput + c] = layout.sourceSpans[c];
		}
		if (AudioCode* code = code_.load(std::memory_order_acquire)) {
			code->process(layout.inputs, layout.outputs, frames, cycle);
		} else {
			for (const std::span<float> out : layout.outputs) std::ranges::fill(out, 0.0f);
		}
		applyFade(layout, frames);  // before metering, so levels show the fade
		applyHolds(layout, frames); // likewise, after the fade
		// Each sink's tap, after the code and the fade (a sink's bank is never faded), from its inside output bank's
		// spans (an unlinked one's scratch included); only while a code runs: a silence binds none.
		for (std::size_t s = 0; s < layout.sinkCount; ++s) {
			const SinkBinding& binding = layout.sinks[s];
			if (binding.tap == nullptr || binding.channels == 0 ||
			    binding.firstOutput + binding.channels > layout.outputs.size())
				continue;
			for (std::size_t c = 0; c < binding.channels; ++c)
				layout.tapSpans[c] = layout.outputs[binding.firstOutput + c];
			const Latency upstream =
				unpacked(layout.outputToldNow[binding.firstOutput].load(std::memory_order_acquire)); // the bank's told
			binding.tap->write(std::span<const std::span<const float>>(layout.tapSpans.data(), binding.channels),
			                   frames, chunkStamp(cycle, upstream));
		}
		// Public ports only: inputs and outputs list the public ports before the inside ones.
		for (std::size_t i = 0; i < layout.publicInputs; ++i) layout.meters[i].add(layout.inputs[i]);
		for (std::size_t i = 0; i < layout.publicOutputs; ++i)
			layout.meters[layout.publicInputs + i].add(layout.outputs[i]);
	}

	void publish(Layout& layout, std::uint64_t windowEnd) noexcept {
		const std::span<Level> next = layout.registered.mailbox.next();
		for (std::size_t i = 0; i < layout.meters.size(); ++i) next[i] = layout.meters[i].take();
		layout.registered.mailbox.publish(windowEnd);
		if (levels_->rungFor.exchange(windowEnd, std::memory_order_acq_rel) != windowEnd) {
			levels_->wake.levelsPublished();
		}
	}

	std::atomic<AudioCode*> code_{nullptr}; // nullptr: every output silent
	Engine& engine_;
	PortBlockEvents& events_;
	pw_loop* loop_;
	NodeName node_;
	AudioFormat format_;
	std::uint32_t maxBlock_;
	std::uint32_t windowFrames_;
	std::vector<float> zeros_; // an unlinked input reads these
	std::shared_ptr<Levels> levels_;
	// Main thread only: the block's ports (its last reshape's, else its making's), its ports in the engine by key, the
	// ports the latency plan was worked out for, what each port's peers report and what each port told them last (by
	// handle), the plan, and the facts it last worked out.
	Ports target_;
	std::map<PortKey, void*> engine_ports_;
	Ports latencyPorts_;
	std::map<void*, std::optional<Latency>> peers_;
	std::map<void*, Latency> told_;
	LatencyPlan plan_;
	LatencyFacts facts_;
	SampleRate rate_;
	const CutRecord& cuts_;       // the engine's record of the internal clock's cuts
	std::uint32_t internalClock_; // the internal clock's id: its cycles are numbered by cuts_
	CycleAccount account_;        // each cycle's facts, from the clock as the realtime side last saw it; data loop only
	FrameClock cycleClock_;       // the graph's, for how long a cycle lasts (cycleOverran)
	CycleCounters counters_;      // the node's counters, written on the data loop, read on any thread
	static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
	              "the realtime side reads packed latencies lock-free");
	bool readyTold_ = false;
	bool lostTold_ = false;
	bool leaving_ = false; // faulted: the filter goes at the next refusal event
	std::string refusalDetail_;

	// The asynchronous drive and fade (see pump()).
	enum class Kind : std::uint8_t { drive, fade, hold, release, swap, layout };
	// A first choice: one invoke in flight per block, the rest queued here (docs/engine.md#drive-and-fade).
	static constexpr std::size_t kRing = 8;
	/// One request in the ring: what the main thread hands, then what the data loop writes back.
	struct Request {
		Kind kind = Kind::drive;
		AudioCode* code = nullptr;
		const bool* faded = nullptr;
		std::size_t fadedCount = 0;
		const std::uint8_t* outputs = nullptr; // a hold or a release: the outputs it marks
		std::size_t outputCount = 0;
		const SinkBinding* sinks = nullptr; // a drive: the taps it binds
		std::size_t sinkCount = 0;
		const SourceBinding* sources = nullptr; // a drive: the sources it binds
		std::size_t sourceCount = 0;
		Layout* layout = nullptr;  // installed before the request applies; null: the layout stays
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
		std::vector<std::uint8_t> outputs; // a hold or a release: what the ring's request points into
		Layout* layout;                    // what the request installs, kept in layouts_ until a later one is installed
	};
	BlockFade fade_;                 // data loop only
	std::uint64_t holdsAckedRt_ = 0; // holds acknowledged; data loop only
	Layout* rt_ = nullptr;           // the layout every cycle reads; data loop only, once connected
	std::array<Request, kRing> ring_{};
	std::atomic<std::uint64_t> published_{0}; // requests the main thread pushed into the ring
	std::atomic<std::uint64_t> drained_{0};   // requests the data loop applied
	std::atomic<std::uint64_t> completed_{0}; // requests the data loop completed
	std::atomic<std::uint64_t> takenAt_{0};
	std::atomic<bool> processing_{false}; // STREAMING, as the main loop last heard
	std::atomic<std::uint64_t> invokesRun_{0};
	std::atomic<std::uint64_t> holdsAcked_{0}; // holds the data loop acknowledged
	// Main thread only.
	std::deque<Pending> pending_;                          // from request finished_ on
	std::deque<std::move_only_function<void()>> holdAcks_; // each hold's acknowledgement not yet run, in order
	std::uint64_t holdAcksRun_ = 0;
	std::uint64_t pushed_ = 0;
	std::uint64_t finished_ = 0;
	std::uint64_t invokesPosted_ = 0;
	DataLoop postedLoop_; // the loop the last invoke was posted to: the block's data loop
	bool kick_ = false;
	std::uint32_t blockId_ = SPA_ID_INVALID;
	AudioCode* lastCode_ = nullptr; // the code of the last drive or swap asked: a reshape keeps its view
	Layout* installed_ = nullptr;   // the layout the data loop reads once every completed request was settled
	Layout* lastAsked_ = nullptr;   // the layout the data loop reads once every request asked is applied
	std::shared_ptr<int> life_ = std::make_shared<int>(0); // a `done` that destroyed the block expires it
	// What the constructor registers, in its order and declared last, so a throw in the constructor unwinds them (in
	// reverse: the listener before its filter, the levels' entry before the layouts it reads) before any member they
	// reach goes; the destructor removes each first, in its own order.
	std::deque<std::unique_ptr<Layout>> layouts_; // from the installed one on, in the order asked
	LoopSource refusal_;
	LoopSource completion_;
	OwnedFilter filter_;
	Listener listener_;
	LevelsEntry levelsEntry_;
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
