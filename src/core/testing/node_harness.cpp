// The node harness (ear/testing/node_harness.hpp): one real NodeHost over in-memory fakes, on the test's one thread
// (docs/writing-a-node-type.md#testing-a-type). Each fake is private to this file. Where a fake must act as the engine
// does it calls the engine's own rule, never a copy: the block's fade and when its fade-outs complete, its cycle facts
// (CycleAccount, ear/core/cycle_facts.hpp) and stamps (ear/core/block_rules.hpp), its latency (workOut), a sink's rings
// (SinkRing), a source's (SourceRing), and an engine companion's bank as NodeHost names it
// (CompanionTarget::insideBank).
#include "ear/testing/node_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "../legacy_instance.hpp"
#include "ear/core/block_rules.hpp"
#include "ear/core/cycle_counters.hpp"
#include "ear/core/cycle_facts.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/core/sink_ring.hpp"
#include "ear/core/source_ring.hpp"
#include "ear/testing/harness_cuts.hpp"

namespace ear::testing {
namespace {

/// The driver the harness's cycles run on until changeDriver: the internal clock, the one whose cycles are numbered.
constexpr std::uint32_t internalClock = 0;

/// The main loop: queues posts, from any thread; the harness runs them on the test's thread until none is left.
class ManualExecutor final : public Executor {
public:
	void post(std::move_only_function<void()> work) override {
		const std::scoped_lock lock(mutex_);
		queue_.push_back(std::move(work));
	}
	/// Runs everything posted, and what that posts, until nothing is left.
	void pump() {
		for (;;) {
			std::move_only_function<void()> next;
			{
				const std::scoped_lock lock(mutex_);
				if (queue_.empty()) return;
				next = std::move(queue_.front());
				queue_.pop_front();
			}
			next();
		}
	}

private:
	std::mutex mutex_;
	std::deque<std::move_only_function<void()>> queue_;
};

/// Main-loop timers on a clock the test moves.
class FakeClock final : public Timers {
public:
	explicit FakeClock(ManualExecutor& executor) : executor_(executor) {}
	std::unique_ptr<Timer> after(std::chrono::milliseconds delay, std::move_only_function<void()> due) override {
		auto armed = std::make_shared<Armed>(Armed{now_ + delay, std::move(due)});
		armed_.push_back(armed);
		return std::make_unique<Handle>(armed);
	}
	std::chrono::steady_clock::time_point now() const override { return std::chrono::steady_clock::time_point{now_}; }
	/// Moves the clock and runs what came due, in due order, pumping between.
	void advance(std::chrono::milliseconds by) {
		const auto until = now_ + by;
		for (;;) {
			std::shared_ptr<Armed> next;
			for (const auto& armed : armed_)
				if (!armed->cancelled && armed->due && armed->at <= until && (!next || armed->at < next->at))
					next = armed;
			if (!next) break;
			now_ = std::max(now_, next->at);
			auto call = std::move(next->due);
			next->due = nullptr;
			call();
			executor_.pump();
		}
		now_ = until;
		std::erase_if(armed_, [](const auto& armed) { return armed->cancelled || !armed->due; });
	}

private:
	struct Armed {
		std::chrono::milliseconds at;
		std::move_only_function<void()> due;
		bool cancelled = false;
	};
	struct Handle final : Timer {
		explicit Handle(std::shared_ptr<Armed> a) : armed(std::move(a)) {}
		~Handle() override { armed->cancelled = true; }
		std::shared_ptr<Armed> armed;
	};
	ManualExecutor& executor_;
	std::chrono::milliseconds now_{0};
	std::vector<std::shared_ptr<Armed>> armed_;
};

/// A request a fake block was asked, completed at once (autoComplete) or when the test says.
struct PendingRequest {
	bool fade;
	std::move_only_function<void()> done;
};

} // namespace

struct NodeHarness::Impl final : NodeHealthListener,
								 NodeNoteListener,
								 NodeDiagnosticListener,
								 EngineDiagnosticListener {
	// ---- The fakes ----

	/// The node's one block: a drive applies its code at once (its fade-in and its first cycle's discontinuity with
	/// it), a fade-out ramps the faded outputs down from the next cycle. A drive or silence completes on the main loop at
	/// once (autoComplete) or when the test says. A fade-out completes by the real block's rule (BlockFade): at once
	/// with autoComplete (a block not processing), else when a cycle ends it at zero or the test acknowledges it (a
	/// block not processing then), and a drive applied while it waits cancels it (its `done` never runs).
	struct FakeBlock final : PortBlock {
		explicit FakeBlock(Impl& h) : harness(h) {}
		~FakeBlock() override { harness.block = nullptr; }
		void drive(DriveRequest request, std::move_only_function<void(std::uint64_t)> done) override {
			harness.driven = request.code;
			harness.bound.assign(request.sinks.begin(), request.sinks.end()); // the taps change with the code
			harness.boundSources.assign(request.sources.begin(), request.sources.end());
			std::ranges::fill(harness.fadeMask, std::uint8_t{0});
			for (std::size_t i = 0; i < harness.fadeMask.size() && i < request.faded.size(); ++i)
				harness.fadeMask[i] = request.faded[i] ? 1 : 0;
			const std::uint64_t cancelled = harness.fade.driven(
				std::ranges::any_of(harness.fadeMask, [](std::uint8_t faded) { return faded != 0; }));
			harness.dropFades(cancelled);
			harness.account.driven();
			const std::uint64_t taken = harness.account.nextPosition();
			harness.request(false, [done = std::move(done), taken]() mutable { done(taken); });
		}
		void fadeOut(std::move_only_function<void()> done) override {
			harness.fade.fadeOut();
			harness.pending.push_back(PendingRequest{true, std::move(done)});
			if (harness.options.autoComplete) harness.completeFades(harness.fade.notProcessing());
		}
		void carryLatency(const LatencyPlan& plan) override {
			// As the real block: worked out at once (a plan that does not fit the ports throws here), with no peer on
			// any port, since nothing is linked to the harness's block.
			const Ports& ports = harness.config->ports();
			const std::vector<std::optional<Latency>> inputs(ports.inputs().size() + ports.insideInputs().size());
			const std::vector<std::optional<Latency>> outputs(ports.outputs().size() + ports.insideOutputs().size());
			harness.facts = workOut(ports, plan, inputs, outputs);
			harness.plan = plan;
		}
		void settle() override {}
		std::uint64_t takenAt() const noexcept override { return harness.position; }
		std::uint32_t blockId() const noexcept override { return 7; }
		NodeCounts counts() const noexcept override { return counters.read(); }
		Impl& harness;
		CycleCounters counters; // its node counters, as the real block counts them (process())
	};
	struct FakeBlocks final : PortBlockFactory {
		explicit FakeBlocks(Impl& h) : harness(h) {}
		std::unique_ptr<PortBlock> make(const NodeName&, const Ports&, const AudioFormat&,
		                                PortBlockEvents& events) override {
			harness.blockEvents = &events;
			auto made = std::make_unique<FakeBlock>(harness);
			harness.block = made.get();
			return made;
		}
		Impl& harness;
	};

	/// An engine companion: its fake is marked destroyed with it.
	struct FakeEngineCompanion final : EngineCompanion {
		FakeEngineCompanion(Impl& h, std::size_t i) : harness(h), index(i) {}
		~FakeEngineCompanion() override {
			harness.companionList.at(index).destroyed = true;
			harness.engineEvents.at(index) = nullptr;
		}
		Impl& harness;
		std::size_t index;
	};
	template <typename Shape> struct FakeEngineCompanions final : EngineCompanionAdapter<Shape> {
		explicit FakeEngineCompanions(Impl& h) : harness(h) {}
		std::unique_ptr<EngineCompanion> make(const CompanionTarget& target, const Shape& shape,
		                                      EngineCompanionEvents& events) override {
			CompanionKind kind = CompanionKind::rtpReceiver;
			if constexpr (std::is_same_v<Shape, SoundCardShape>)
				kind = target.bank == Direction::output ? CompanionKind::cardPlayback : CompanionKind::cardCapture;
			if (target.insideBank.empty()) throw std::logic_error("an engine companion asked for on no inside bank");
			harness.companionList.push_back(
				CompanionFake{kind, target.insideBank, target.engineNode, shape, true, false, false, {}});
			harness.engineEvents.push_back(&events);
			return std::make_unique<FakeEngineCompanion>(harness, harness.companionList.size() - 1);
		}
		Impl& harness;
	};

	/// A sink over core's own SinkRing (docs/writing-a-node-type.md#testing-a-type), its consumer on the test's thread:
	/// begin calls the consumer's begin at once (or, with holdSinks, at ready(bank)), then posts `begun`; the tap
	/// writes the ring by its rules (the two rings dropping together) and drains it at once, so the consumer gets each
	/// chunk as the engine's sink hands it, in two calls where it wraps the ring's end, each call copied into the
	/// companion; close finishes a consumer that began, marks the companion destroyed and posts `closed` with the
	/// frames dropped.
	struct FakeSink final : Sink, SinkTap {
		FakeSink(Impl& h, std::size_t i, std::uint32_t channels, std::uint32_t frames, std::uint32_t rate)
			: harness(h), index(i), rules(channels, frames, rate) {
			harness.sinkFakes[index] = this;
		}
		~FakeSink() override { harness.sinkFakes.erase(index); }
		SinkTap& tap() noexcept override { return *this; }
		void begin(std::unique_ptr<SinkConsumer> c, std::move_only_function<void()> b) override {
			consumer = std::move(c);
			begun = std::move(b);
			if (!harness.options.holdSinks) runBegin();
		}
		/// The sink's thread runs the consumer's begin, then posts `begun`.
		void runBegin() {
			consumer->begin();
			began = true;
			harness.companionList.at(index).ready = true;
			harness.executor.post(std::exchange(begun, nullptr));
		}
		void close(std::move_only_function<void(std::uint64_t)> closed) override {
			drain();
			if (began) consumer->finish();
			finished = true;
			harness.companionList.at(index).destroyed = true;
			harness.executor.post(
				[closed = std::move(closed), dropped = rules.droppedFrames()]() mutable { closed(dropped); });
		}
		std::uint64_t droppedFrames() const noexcept override { return rules.droppedFrames(); }
		void write(std::span<const std::span<const float>> bank, std::uint32_t frames,
		           const ChunkStamp& stamp) noexcept override {
			if (rules.write(bank, frames, stamp)) drain();
		}
		/// What the sink's thread does when woken: every chunk to a consumer that began and has not finished, else
		/// skipped.
		void drain() {
			if (!began || finished) return rules.discard();
			rules.drain([&](const Chunk& chunk) {
				harness.companionList.at(index).chunks.push_back(
					SeenChunk{{chunk.interleaved.begin(), chunk.interleaved.end()},
				              chunk.frames,
				              ChunkStamp{.position = chunk.position,
				                         .startNs = chunk.startNs,
				                         .startRawNs = chunk.startRawNs,
				                         .causes = chunk.causes,
				                         .lostFrames = chunk.lostFrames,
				                         .upstreamFrames = chunk.upstreamFrames}});
				consumer->consume(chunk);
			});
		}
		Impl& harness;
		std::size_t index;
		SinkRing rules;
		std::unique_ptr<SinkConsumer> consumer;
		std::move_only_function<void()> begun; // until its consumer begins
		bool began = false;
		bool finished = false;
	};
	struct FakeSinks final : SinkAdapter {
		explicit FakeSinks(Impl& h) : harness(h) {}
		std::unique_ptr<Sink> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
		                           std::uint32_t capacityFrames, std::uint32_t rate) override {
			CompanionFake fake{CompanionKind::sink,
			                   std::string(bank),
			                   node + "." + std::string(bank),
			                   SinkShape{.frames = capacityFrames},
			                   true,
			                   false,
			                   false,
			                   {}};
			harness.companionList.push_back(std::move(fake));
			harness.engineEvents.push_back(nullptr); // a sink has no engine companion's events
			return std::make_unique<FakeSink>(harness, harness.companionList.size() - 1, channels,
			                                  harness.options.sinkRingFrames.value_or(capacityFrames), rate);
		}
		Impl& harness;
	};

	/// A source over core's own SourceRing, so a type's tests play by the engine's rules
	/// (docs/writing-a-node-type.md#sources): its writer offers what fits and never waits, an idle returns at once, and
	/// nobody watches its played wake (it is never rung: the ring's signals are ignored).
	struct FakeSource final : Source, SourceWriter, SourceRing::Signals, Wake {
		FakeSource(Impl& h, std::size_t i, std::uint32_t channels, std::uint32_t frames, std::uint32_t rate)
			: harness(h), index(i), rules(channels, frames, rate, *this) {}
		~FakeSource() override { harness.companionList.at(index).destroyed = true; }
		SourceWriter& writer() noexcept override { return *this; }
		SourceTap& tap() noexcept override { return rules; }
		void stopped() noexcept override { rules.dropForStop(); }
		void stopping() noexcept override { rules.markIdle(); }
		void close() noexcept override { rules.close(); }
		std::uint64_t shortfallFrames() const noexcept override { return rules.shortfallFrames(); }
		std::uint64_t droppedFrames() const noexcept override { return rules.droppedFrames(); }
		std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames,
		                    std::chrono::milliseconds) override {
			return rules.offer(interleaved, frames);
		}
		void idle(std::chrono::milliseconds) override { rules.markIdle(); }
		void clear() noexcept override { rules.clear(); }
		bool closed() const noexcept override { return rules.closed(); }
		std::uint32_t capacityFrames() const noexcept override { return rules.capacityFrames(); }
		Played played() const noexcept override { return rules.played(); }
		Wake& playedWake() noexcept override { return *this; }
		void ring() noexcept override {}
		int descriptor() const noexcept override { return -1; }
		void answer() noexcept override {}
		void roomFreed() noexcept override {}
		void playedChanged() noexcept override {}
		Impl& harness;
		std::size_t index;
		SourceRing rules;
	};
	struct FakeSources final : SourceAdapter {
		explicit FakeSources(Impl& h) : harness(h) {}
		std::unique_ptr<Source> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
		                             std::uint32_t capacityFrames, std::uint32_t rate) override {
			harness.companionList.push_back(CompanionFake{CompanionKind::source,
			                                              std::string(bank),
			                                              node + "." + std::string(bank),
			                                              SourceShape{.frames = capacityFrames},
			                                              true,
			                                              true,
			                                              false,
			                                              {}});
			harness.engineEvents.push_back(nullptr); // a source has no engine companion's events
			auto made =
				std::make_unique<FakeSource>(harness, harness.companionList.size() - 1, channels, capacityFrames, rate);
			harness.sourceList.push_back(made.get());
			return made;
		}
		Impl& harness;
	};

	/// A legacy handoff whose writes go straight to its consumer, on the writing thread (the test's).
	struct FakeHandoff final : Handoff, RtWriter {
		FakeHandoff(Impl& h, std::size_t i, HandoffConsumer& c, std::size_t n)
			: harness(h), index(i), consumer(c), channels(n) {}
		RtWriter& writer() noexcept override { return *this; }
		std::uint32_t write(std::span<const std::span<const float>> planar, std::uint32_t count) noexcept override {
			interleaved.assign(static_cast<std::size_t>(count) * channels, 0.0F);
			for (std::size_t channel = 0; channel < channels && channel < planar.size(); ++channel)
				for (std::uint32_t frame = 0; frame < count; ++frame)
					interleaved[frame * channels + channel] = planar[channel][frame];
			consumer.consume(interleaved, count);
			harness.handoffCounts.at(index) += count;
			return count;
		}
		void close(std::move_only_function<void(std::uint64_t)> closed) override {
			consumer.finish();
			harness.executor.post([closed = std::move(closed)]() mutable { closed(0); });
		}
		Impl& harness;
		std::size_t index;
		HandoffConsumer& consumer;
		std::size_t channels;
		std::vector<float> interleaved;
	};
	struct FakeHandoffs final : HandoffAdapter {
		explicit FakeHandoffs(Impl& h) : harness(h) {}
		std::unique_ptr<Handoff> make(const NodeName&, std::uint32_t channels, std::uint32_t,
		                              HandoffConsumer& consumer) override {
			harness.handoffCounts.push_back(0);
			if (!harness.handoffList.empty()) harness.handoffList.back().channels = channels;
			return std::make_unique<FakeHandoff>(harness, harness.handoffCounts.size() - 1, consumer, channels);
		}
		Impl& harness;
	};

	/// Every config the type parses, wrapped so the harness sees the instance each configuration makes (a legacy run
	/// object, for runObject()) and when it goes.
	/// Both contexts, forwarded to NodeHost's, recording the requests the harness shows (handoffs, recordings). Lives
	/// only during configure, as the contexts it forwards to are valid only then.
	struct SeenContexts final : CreateContext, StartContext {
		SeenContexts(Impl& h, InstanceContexts& real) : harness(h), create(real.create), legacy(real.legacy) {}
		AudioFormat format() const override { return create.format(); }
		const NodeName& node() const override { return create.node(); }
		const Ports& ports() const override { return create.ports(); }
		void use(NodeCode code) override { create.use(std::move(code)); }
		void soundCard(std::string_view bank, SoundCardShape shape) override {
			create.soundCard(bank, std::move(shape));
		}
		void rtpReceiver(std::string_view bank, RtpReceiverShape shape) override {
			create.rtpReceiver(bank, std::move(shape));
		}
		std::expected<int, std::string> openRecording(const RecordingRequest& request) override {
			harness.recordingList.push_back(request);
			return create.openRecording(request);
		}
		SinkRequest& sink(std::string_view bank, SinkShape shape) override { return create.sink(bank, shape); }
		SourceWriter& source(std::string_view bank, SourceShape shape) override { return create.source(bank, shape); }
		void declareLatency(std::string_view from, std::string_view to, Latency latency) override {
			create.declareLatency(from, to, latency);
		}
		[[noreturn]] void refuse(std::string rule) override {
			create.refuse(rule);
			throw std::logic_error(rule); // never reached: refuse throws
		}
		InsidesEvents& events() override { return legacy.events(); }
		void declareLatency(const std::string& bank, Latency internal) override {
			legacy.declareLatency(bank, internal);
		}

	protected:
		Acquired acquireResource(const ResourceRequest& request) override {
			if (const auto* handoff = std::get_if<HandoffRequest>(&request)) {
				harness.handoffList.push_back(SeenHandoff{handoff->bank, 0, handoff->capacityFrames});
				return &legacy.acquire(*handoff);
			}
			const auto& recording = std::get<RecordingRequest>(request);
			harness.recordingList.push_back(recording);
			return legacy.acquire(recording);
		}

	public:
		Impl& harness;
		CreateContext& create;
		StartContext& legacy;
	};

	struct SeenInstance final : ErasedInstance {
		SeenInstance(Impl& h, std::unique_ptr<ErasedInstance> i) : harness(h), inner(std::move(i)) {
			harness.instance = this;
		}
		~SeenInstance() override {
			if (harness.instance == this) harness.instance = nullptr;
		}
		Configured configure(InstanceContexts& cx) override {
			SeenContexts seen(harness, cx);
			InstanceContexts contexts{seen, seen};
			return inner->configure(contexts);
		}
		bool hasRecipe() const noexcept override { return inner->hasRecipe(); }
		bool engineRefused() const noexcept override { return inner->engineRefused(); }
		std::expected<void, FailureReason> ready(Ready& ready) override { return inner->ready(ready); }
		void start() override { inner->start(); }
		void stop() override { inner->stop(); }
		void close(std::move_only_function<void()> closed) override { inner->close(std::move(closed)); }
		Impl& harness;
		std::unique_ptr<ErasedInstance> inner;
	};
	struct SeenConfig final : NodeConfig {
		SeenConfig(Impl& h, std::unique_ptr<const NodeConfig> c) : harness(h), inner(std::move(c)) {}
		const Ports& ports() const override { return inner->ports(); }
		StartResult start(StartContext& context) const override { return inner->start(context); }
		std::unique_ptr<ErasedInstance> make(NodeContext& node) const override {
			return std::make_unique<SeenInstance>(harness, inner->make(node));
		}
		Impl& harness;
		std::unique_ptr<const NodeConfig> inner;
	};

	Impl(const NodeType& t, Options o)
		: type(t), options(std::move(o)), clock(executor), blocks(*this), cards(*this), receivers(*this),
		  handoffs(*this), sinks(*this), sources(*this),
		  registry(options.recordings != nullptr
	                   ? ResourceRegistry(cards, receivers, handoffs, sinks, sources, *options.recordings)
	                   : ResourceRegistry(cards, receivers, handoffs, sinks, sources)),
		  graph(catalogue), fade(options.format.rate), account(FrameClock(options.format.rate), &cuts),
		  host(std::make_unique<NodeHost>(options.format, blocks, registry, executor, clock)) {
		host->subscribe(*this);
		host->subscribeNotes(*this);
		host->subscribeDiagnostics(*this);
		host->subscribeEngineDiagnostics(*this);
	}

	// ---- Listeners ----
	void nodeChanged(const NodeHealth& h) override { told = h; }
	void nodeGone(const NodeName&, std::uint64_t dropped) override {
		wentAway = true;
		dropped_ = dropped;
	}
	void nodeSays(const NodeName&, const std::string& text) override { noteList.push_back(text); }
	void nodeDiagnostic(const NodeName&, std::string_view code, const std::string& detail,
	                    std::span<const DiagnosticField> fields) override {
		SeenDiagnostic seen{std::string(code), detail, {}};
		for (const DiagnosticField& field : fields) seen.fields.emplace_back(std::string(field.key), field.value);
		diagnosticList.push_back(std::move(seen));
	}
	void engineDiagnostic(const EngineDiagnostic& diagnostic) override {
		SeenDiagnostic seen{std::string(diagnostic.code), diagnostic.detail, {}};
		for (const DiagnosticField& field : diagnostic.fields)
			seen.fields.emplace_back(std::string(field.key), field.value);
		diagnosticList.push_back(std::move(seen));
	}

	// ---- Requests ----
	/// A drive or a silence: completed at once (autoComplete) or when the test says.
	void request(bool isFade, std::move_only_function<void()> done) {
		if (options.autoComplete) executor.post(std::move(done));
		else pending.push_back(PendingRequest{isFade, std::move(done)});
	}
	void completeOldest(bool isFade) {
		const auto found = std::ranges::find_if(pending, [&](const auto& one) { return one.fade == isFade; });
		if (found == pending.end())
			throw std::logic_error(isFade ? "no fade-out is pending" : "no drive or silence is pending");
		auto done = std::move(found->done);
		pending.erase(found);
		executor.post(std::move(done));
		executor.pump();
	}
	/// The oldest `count` fade-outs pending, which BlockFade completed: each `done` is posted (acknowledged), or none
	/// is (cancelled by a drive). Never pumped here: a fake's call inside NodeHost's turn only posts, as the real block
	/// signals.
	void finishFades(std::uint64_t count, bool acknowledged) {
		for (auto it = pending.begin(); it != pending.end() && count > 0;) {
			if (!it->fade) {
				++it;
				continue;
			}
			if (acknowledged) executor.post(std::move(it->done));
			it = pending.erase(it);
			--count;
		}
	}
	void completeFades(std::uint64_t count) {
		if (count > 0) finishFades(count, true);
	}
	void dropFades(std::uint64_t count) {
		if (count > 0) finishFades(count, false);
	}

	// ---- The node ----
	NodeSpec spec(bool disabled) const { return NodeSpec{options.node, std::string(type.name()), settings, disabled}; }
	void requireParsed() const {
		if (!config) throw std::logic_error("the harness has no parsed settings");
	}
	void requireAdded() const {
		if (!added) throw std::logic_error("the harness has added no node");
	}
	void buffers() {
		const Ports& ports = config->ports();
		inputNames.clear();
		outputNames.clear();
		for (const auto* names : {&ports.inputs(), &ports.insideInputs()})
			inputNames.insert(inputNames.end(), names->begin(), names->end());
		for (const auto* names : {&ports.outputs(), &ports.insideOutputs()})
			outputNames.insert(outputNames.end(), names->begin(), names->end());
		inputs.assign(inputNames.size(), std::vector<float>(options.format.maxBlock, 0.0F));
		outputs.assign(outputNames.size(), std::vector<float>(options.format.maxBlock, 0.0F));
		lastGain.assign(outputNames.size(), 1.0F);
		fadeMask.assign(outputNames.size(), 0);
	}
	/// What the real block with no peers tells on output port `output` (outputs then insideOutputs; its capture latency)
	/// and on input port `input` (its playback latency): workOut's, as the plan was last carried
	/// (docs/engine.md#latency-per-bank-pair).
	Latency toldOnOutput(std::size_t output) const {
		const Ports& ports = config->ports();
		return facts.told.empty() ? Latency{}
		                          : facts.told.at(ports.inputs().size() + ports.insideInputs().size() + output);
	}
	Latency toldOnInput(std::size_t input) const { return facts.told.empty() ? Latency{} : facts.told.at(input); }
	/// The newest source made on `bank`.
	FakeSource& sourceOn(std::string_view bank) const {
		for (auto it = sourceList.rbegin(); it != sourceList.rend(); ++it)
			if (companionList.at((*it)->index).bank == bank) return **it;
		throw std::logic_error("no source was made on bank " + std::string(bank));
	}
	static std::size_t indexOf(const std::vector<PortName>& names, std::string_view port) {
		const auto found = std::ranges::find(names, port);
		if (found == names.end()) throw std::logic_error("the node has no port " + std::string(port));
		return static_cast<std::size_t>(found - names.begin());
	}
	/// Teardown to the end: remove the node (stopAll), completing every request the test left pending.
	void finish() {
		if (!added || stopped) return;
		stopped = true;
		host->stopAll(graph.close(), [] {});
		executor.pump();
		fade.notProcessing(); // the fade-outs pending complete with the rest
		for (int rounds = 0; rounds < 1000 && !pending.empty(); ++rounds) {
			auto done = std::move(pending.front().done);
			pending.pop_front();
			executor.post(std::move(done));
			executor.pump();
		}
		clock.advance(defaultCloseDeadline); // a legacy run past its close deadline is leaked, never waited for
	}

	const NodeType& type;
	Options options;
	ManualExecutor executor;
	FakeClock clock;
	FakeBlocks blocks;
	FakeEngineCompanions<SoundCardShape> cards;
	FakeEngineCompanions<RtpReceiverShape> receivers;
	FakeHandoffs handoffs;
	FakeSinks sinks;
	FakeSources sources;
	ResourceRegistry registry;
	NodeCatalogue catalogue{{}};
	DesiredGraph graph; // only for close(), the proof stopAll takes

	NodeSettings settings;
	std::shared_ptr<const NodeConfig> config;
	bool added = false;
	bool disabled = false;
	bool stopped = false;
	SeenInstance* instance = nullptr;

	FakeBlock* block = nullptr;
	PortBlockEvents* blockEvents = nullptr;
	AudioCode* driven = nullptr;
	std::vector<SinkBinding> bound;          // the taps the last drive bound (none for a silence)
	std::vector<SourceBinding> boundSources; // the sources the last drive bound (none for a silence)
	LatencyPlan plan;
	LatencyFacts facts; // workOut's for `plan`, with no peers; empty before the first plan
	std::deque<PendingRequest> pending;
	std::vector<std::uint8_t> fadeMask; // per output, outputs then insideOutputs: the last drive's faded ones
	BlockFade fade;                     // the real block's fade and its rule for fade-outs
	// The fake block's cycles (docs/writing-a-node-type.md#lost-cycles): its graph's cut record, the real block's rule
	// that states each cycle's facts, and the clock the next process() runs on.
	HarnessCuts cuts;
	CycleAccount account;
	std::uint64_t position = 0;   // the driver's frames at the next cycle's start
	std::uint64_t timeFrames = 0; // the raw clock at the next cycle's start, in frames since the harness began
	std::uint32_t driverId = internalClock;
	std::uint32_t lostCut = 0;         // cycles loseCycles lost since the last process(), cut short
	std::uint32_t lostUnscheduled = 0; // and those the block was not scheduled for
	CycleFacts lastFacts;

	std::vector<CompanionFake> companionList;
	std::vector<EngineCompanionEvents*> engineEvents; // per companion; null once destroyed (or none)
	std::vector<FakeSource*> sourceList;              // every source made, in order (a freed one dangles: unused)
	std::map<std::size_t, FakeSink*> sinkFakes;       // each sink alive, by its companion's index
	std::vector<std::uint64_t> handoffCounts;
	std::vector<SeenHandoff> handoffList;
	std::vector<RecordingRequest> recordingList;

	std::optional<NodeHealth> told;
	bool wentAway = false;
	std::uint64_t dropped_ = 0;
	std::vector<std::string> noteList;
	std::vector<SeenDiagnostic> diagnosticList;

	std::vector<PortName> inputNames;
	std::vector<PortName> outputNames;
	std::vector<std::vector<float>> inputs;
	std::vector<std::vector<float>> outputs;
	std::vector<float> lastGain;
	std::uint32_t lastFrames = 0;
	// Last, so it goes first: its nodes' blocks, engine companions and instances tell the members above as they go.
	std::unique_ptr<NodeHost> host;
};

NodeHarness::NodeHarness(const NodeType& type, Options options)
	: impl_(std::make_unique<Impl>(type, std::move(options))) {}

NodeHarness::~NodeHarness() {
	try {
		impl_->finish();
	} catch (...) {
		// A type bug thrown while tearing down: the test already failed on it, or will on what it checks.
	}
}

std::optional<std::string> NodeHarness::parse(const NodeSettings& settings) {
	auto parsed = impl_->type.parse(impl_->options.node, settings);
	if (!parsed) return parsed.error().detail;
	impl_->settings = settings;
	impl_->config = std::make_shared<const Impl::SeenConfig>(*impl_, std::move(*parsed));
	impl_->buffers();
	return std::nullopt;
}

const Ports& NodeHarness::ports() const {
	impl_->requireParsed();
	return impl_->config->ports();
}

void NodeHarness::add(AddOptions options) {
	impl_->requireParsed();
	if (impl_->added) throw std::logic_error("the harness adds one node");
	impl_->added = true;
	impl_->disabled = options.disabled;
	impl_->host->nodeAdded(impl_->spec(options.disabled), impl_->config);
	impl_->executor.pump();
}

void NodeHarness::blockReady() {
	if (impl_->blockEvents == nullptr) throw std::logic_error("no block was made");
	impl_->blockEvents->ready();
	impl_->executor.pump();
}

void NodeHarness::blockLost(std::string detail) {
	if (impl_->blockEvents == nullptr) throw std::logic_error("no block was made");
	impl_->blockEvents->lost(std::move(detail));
	impl_->executor.pump();
}

void NodeHarness::ready(std::string_view bank) {
	CompanionFake& fake = companion(bank);
	const auto index = static_cast<std::size_t>(&fake - impl_->companionList.data());
	if (fake.kind == CompanionKind::sink) {
		const auto alive = impl_->sinkFakes.find(index);
		if (alive == impl_->sinkFakes.end()) throw std::logic_error("no sink on " + std::string(bank) + " is alive");
		if (!alive->second->began) alive->second->runBegin();
		impl_->executor.pump();
		return;
	}
	if (fake.kind == CompanionKind::source) return; // ready when made
	EngineCompanionEvents* events = impl_->engineEvents.at(index);
	if (events == nullptr) throw std::logic_error("no engine companion on " + std::string(bank) + " is alive");
	fake.ready = true;
	events->ready();
	impl_->executor.pump();
}

void NodeHarness::lose(std::string_view bank, std::string detail) {
	CompanionFake& fake = companion(bank);
	if (!isEngineKind(fake.kind))
		throw std::logic_error("the " + std::string(companionKindName(fake.kind)) + " on " + std::string(bank) +
		                       " is never lost by the framework: its type's code says lost through the node's handle");
	const auto index = static_cast<std::size_t>(&fake - impl_->companionList.data());
	EngineCompanionEvents* events = impl_->engineEvents.at(index);
	if (events == nullptr) throw std::logic_error("no engine companion on " + std::string(bank) + " is alive");
	events->lost(std::move(detail));
	impl_->executor.pump();
}

void NodeHarness::acknowledgeFade() {
	// As a block not processing: every fade-out waiting ends at zero now.
	const std::uint64_t acknowledged = impl_->fade.notProcessing();
	if (acknowledged == 0) throw std::logic_error("no fade-out is pending");
	impl_->completeFades(acknowledged);
	impl_->executor.pump();
}

void NodeHarness::completeBarrier() { impl_->completeOldest(false); }

void NodeHarness::advance(std::chrono::milliseconds by) {
	impl_->clock.advance(by);
	impl_->executor.pump();
}

void NodeHarness::process(std::uint32_t frames) {
	impl_->requireParsed();
	Impl& h = *impl_;
	if (frames == 0) frames = h.options.format.maxBlock;
	if (frames > h.options.format.maxBlock) throw std::logic_error("a cycle is at most maxBlock frames");
	std::vector<std::span<const float>> in;
	std::vector<std::span<float>> out;
	for (auto& buffer : h.inputs) in.emplace_back(buffer.data(), frames);
	for (auto& buffer : h.outputs) {
		std::fill_n(buffer.begin(), frames, std::numeric_limits<float>::quiet_NaN());
		out.emplace_back(buffer.data(), frames);
	}
	// The harness's clock starts at zero and runs at exactly the graph's rate (docs/engine.md#dating). Each cycle lost
	// since the last started on it with this cycle's frames (ear forces one quantum), moving the position and the time
	// on by them, as the tracking dummy's catch-up does (docs/writing-a-node-type.md#lost-cycles).
	const FrameClock clock(h.options.format.rate);
	for (std::uint32_t i = 0; i < h.lostCut + h.lostUnscheduled; ++i) {
		h.cuts.start();
		if (i < h.lostCut) h.cuts.incomplete();
		h.position += frames;
		h.timeFrames += frames;
	}
	h.lostCut = 0;
	h.lostUnscheduled = 0;
	// The block's own work, timed as the real block times it, for its counters' overruns (docs/engine.md#counters).
	const auto entered = std::chrono::steady_clock::now();
	h.cuts.start();
	const std::int64_t at = clock.ns(static_cast<std::int64_t>(h.timeFrames));
	const CycleAccount::Step step =
		h.account.next(DriverTick{.driverId = h.driverId,
	                              .graphCycle = h.driverId == internalClock ? h.cuts.started() : 0,
	                              .position = h.position,
	                              .frames = frames,
	                              .startRawNs = at},
	                   h.options.format.maxBlock);
	h.lastFacts = step.facts;
	// The harness's cycles start on time (their overdueNs 0): a stall of its record ends at each process() on its own
	// clock (docs/engine.md#counters).
	if (h.driverId == internalClock) h.cuts.onTime();
	const GraphCycle cycle{.startNs = at,
	                       .position = h.position,
	                       .startRawNs = at,
	                       .driverId = h.driverId,
	                       .nextNs = clock.ns(static_cast<std::int64_t>(h.timeFrames + frames)),
	                       .facts = step.facts};
	// Each source, before the code, into its bank's buffers, as the real block plays it, while a code runs.
	if (h.driven != nullptr)
		for (const SourceBinding& binding : h.boundSources) {
			std::vector<std::span<float>> bank;
			for (std::size_t c = 0; c < binding.channels && binding.firstInput + c < h.inputs.size(); ++c)
				bank.emplace_back(h.inputs[binding.firstInput + c].data(), frames);
			binding.tap->play(bank, sourceCycle(cycle, frames, h.toldOnInput(binding.firstInput)));
		}
	if (h.driven != nullptr) h.driven->process(in, out, frames, cycle);
	else
		for (auto& span : out) std::ranges::fill(span, 0.0F);
	// The fade, by the real block's own rule.
	const BlockFade::Applied faded = h.fade.apply(out, h.fadeMask, frames);
	for (std::size_t o = 0; o < out.size(); ++o) h.lastGain[o] = h.fadeMask[o] != 0 ? faded.lastGain : 1.0F;
	// Each sink's tap after the fade (a sink's bank is never faded), as the real block writes it, while a code runs.
	if (h.driven != nullptr)
		for (const SinkBinding& binding : h.bound) {
			std::vector<std::span<const float>> bank;
			for (std::size_t c = 0; c < binding.channels && binding.firstOutput + c < out.size(); ++c)
				bank.emplace_back(out[binding.firstOutput + c]);
			binding.tap->write(bank, frames, chunkStamp(cycle, h.toldOnOutput(binding.firstOutput)));
		}
	const std::chrono::nanoseconds work = std::chrono::steady_clock::now() - entered;
	if (h.block != nullptr) h.block->counters.add(step.facts, cycleOverran(work.count(), clock, frames));
	h.position += frames;
	h.timeFrames += frames;
	h.lastFrames = frames;
	// A cycle that ended a fade-out at zero acknowledged every fade-out waiting.
	if (faded.acknowledged > 0) {
		h.completeFades(faded.acknowledged);
		h.executor.pump();
	}
}

void NodeHarness::loseCycles(std::uint32_t count, Lost how) {
	Impl& h = *impl_;
	if (count > std::numeric_limits<std::uint32_t>::max() - h.lostCut - h.lostUnscheduled)
		throw std::logic_error("loseCycles() past 2^32 - 1 cycles before one process()");
	(how == Lost::cut ? h.lostCut : h.lostUnscheduled) += count;
}

void NodeHarness::changeDriver(std::uint32_t id) { impl_->driverId = id; }

void NodeHarness::reposition(std::int64_t frames) {
	Impl& h = *impl_;
	if (frames < 0 && static_cast<std::uint64_t>(-frames) > h.position)
		throw std::logic_error("reposition() before the driver's first frame");
	h.position += static_cast<std::uint64_t>(frames);
}

const CutRecord& NodeHarness::cuts() const noexcept { return impl_->cuts; }

const CycleFacts& NodeHarness::facts() const noexcept { return impl_->lastFacts; }

NodeCounts NodeHarness::counts() const {
	const std::optional<NodeCounts> counts = impl_->host->counts(impl_->options.node);
	if (!counts) throw std::logic_error("no node " + impl_->options.node + " is in the harness");
	return *counts;
}

GraphCounts NodeHarness::graphCounts() const noexcept { return impl_->cuts.counts(); }

NodeOutcome NodeHarness::change(const NodeSettings& settings) {
	impl_->requireAdded();
	auto parsed = impl_->type.parse(impl_->options.node, settings);
	if (!parsed) throw std::logic_error("change() with settings the type refuses: " + parsed.error().detail);
	std::vector<std::string> keys;
	for (const auto& [key, value] : settings.items())
		if (!impl_->settings.contains(key) || impl_->settings.at(key) != value) keys.push_back(key);
	for (const auto& [key, value] : impl_->settings.items())
		if (!settings.contains(key)) keys.push_back(key);
	std::ranges::sort(keys);
	impl_->settings = settings;
	impl_->config = std::make_shared<const Impl::SeenConfig>(*impl_, std::move(*parsed));
	impl_->buffers();
	const NodeOutcome outcome = impl_->disabled ? NodeOutcome::stored : NodeOutcome::restart;
	impl_->host->nodeChanged(impl_->spec(impl_->disabled), impl_->config, NodeChange{outcome, keys});
	impl_->executor.pump();
	return outcome;
}

LifecycleReply NodeHarness::stop() {
	impl_->requireAdded();
	const LifecycleReply reply = impl_->host->stop(impl_->options.node);
	impl_->executor.pump();
	return reply;
}

LifecycleReply NodeHarness::start() {
	impl_->requireAdded();
	const LifecycleReply reply = impl_->host->start(impl_->options.node);
	impl_->executor.pump();
	return reply;
}

LifecycleReply NodeHarness::restart() {
	impl_->requireAdded();
	const LifecycleReply reply = impl_->host->restart(impl_->options.node);
	impl_->executor.pump();
	return reply;
}

void NodeHarness::disable() {
	impl_->requireAdded();
	impl_->disabled = true;
	impl_->host->nodeChanged(impl_->spec(true), impl_->config, NodeChange{NodeOutcome::disable, {}});
	impl_->executor.pump();
}

void NodeHarness::enable() {
	impl_->requireAdded();
	impl_->disabled = false;
	impl_->host->nodeChanged(impl_->spec(false), impl_->config, NodeChange{NodeOutcome::enable, {}});
	impl_->executor.pump();
}

void NodeHarness::remove() {
	impl_->requireAdded();
	impl_->host->nodeRemoved(impl_->options.node);
	impl_->executor.pump();
}

NodeHealth NodeHarness::health() const {
	if (!impl_->told) throw std::logic_error("nothing was told of the node");
	return *impl_->told;
}

std::string_view NodeHarness::phase() const { return phaseName(health().phase); }

bool NodeHarness::gone() const noexcept { return impl_->wentAway; }

std::uint64_t NodeHarness::droppedFrames() const { return impl_->dropped_; }

const std::vector<std::string>& NodeHarness::notes() const noexcept { return impl_->noteList; }

const std::vector<SeenDiagnostic>& NodeHarness::diagnostics() const noexcept { return impl_->diagnosticList; }

std::span<float> NodeHarness::input(std::string_view port) {
	return impl_->inputs.at(Impl::indexOf(impl_->inputNames, port));
}

std::span<const float> NodeHarness::output(std::string_view port) const {
	return impl_->outputs.at(Impl::indexOf(impl_->outputNames, port));
}

std::size_t NodeHarness::unwritten() const {
	std::size_t count = 0;
	for (const auto& buffer : impl_->outputs)
		count += static_cast<std::size_t>(
			std::count_if(buffer.begin(), buffer.begin() + impl_->lastFrames, [](float s) { return std::isnan(s); }));
	return count;
}

float NodeHarness::lastFadeGain(std::string_view port) const {
	return impl_->lastGain.at(Impl::indexOf(impl_->outputNames, port));
}

const LatencyPlan& NodeHarness::latencyPlan() const noexcept { return impl_->plan; }

const std::vector<CompanionFake>& NodeHarness::companions() const noexcept { return impl_->companionList; }

CompanionFake& NodeHarness::companion(std::string_view bank) {
	for (auto it = impl_->companionList.rbegin(); it != impl_->companionList.rend(); ++it)
		if (it->bank == bank) return *it;
	throw std::logic_error("no companion was made on bank " + std::string(bank));
}

std::uint32_t NodeHarness::feed(std::string_view bank, std::span<const float> interleaved) {
	Impl::FakeSource& source = impl_->sourceOn(bank);
	const std::size_t channels = ports().bank(bank).ports.size();
	return source.write(interleaved,
	                    static_cast<std::uint32_t>(interleaved.size() / std::max<std::size_t>(channels, 1)),
	                    std::chrono::milliseconds(0));
}

Played NodeHarness::played(std::string_view bank) const { return impl_->sourceOn(bank).played(); }

std::uint64_t NodeHarness::shortfall(std::string_view bank) const { return impl_->sourceOn(bank).shortfallFrames(); }

std::uint64_t NodeHarness::handoffFrames(std::size_t index) const { return impl_->handoffCounts.at(index); }

const std::vector<SeenHandoff>& NodeHarness::handoffs() const noexcept { return impl_->handoffList; }

const std::vector<RecordingRequest>& NodeHarness::recordingRequests() const noexcept { return impl_->recordingList; }

RunObject* NodeHarness::runObject() const noexcept {
	if (impl_->instance == nullptr) return nullptr;
	const auto* legacy = dynamic_cast<const detail::LegacyInstance*>(impl_->instance->inner.get());
	return legacy != nullptr ? legacy->runObject() : nullptr;
}

} // namespace ear::testing
