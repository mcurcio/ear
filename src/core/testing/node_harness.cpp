// The node harness (ear/testing/node_harness.hpp): one real NodeHost over in-memory fakes, on the test's one thread
// (docs/writing-a-node-type.md#testing-a-type). Each fake is private to this file.
#include "ear/testing/node_harness.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "../legacy_instance.hpp"
#include "ear/core/lifecycle.hpp"

namespace ear::testing {
namespace {

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

	/// The node's one block: a drive applies its code at once (and resets the fade-in), a fade-out silences the faded
	/// outputs from the next cycle; each request completes on the main loop, at once (autoComplete) or when the test
	/// says, in the order asked.
	struct FakeBlock final : PortBlock {
		explicit FakeBlock(Impl& h) : harness(h) {}
		~FakeBlock() override { harness.block = nullptr; }
		void drive(DriveRequest request, std::move_only_function<void(std::uint64_t)> done) override {
			harness.driven = request.code;
			harness.bound.assign(request.sinks.begin(), request.sinks.end()); // the taps change with the code
			std::ranges::fill(harness.fadeMask, false);
			for (std::size_t i = 0; i < harness.fadeMask.size() && i < request.faded.size(); ++i)
				harness.fadeMask[i] = request.faded[i];
			harness.fade = std::ranges::any_of(harness.fadeMask, std::identity{}) ? Fade::in : Fade::none;
			harness.fadeAt = 0;
			const std::uint64_t taken = harness.position;
			harness.request(false, [done = std::move(done), taken]() mutable { done(taken); });
		}
		void fadeOut(std::move_only_function<void()> done) override {
			harness.request(true, [&h = harness, done = std::move(done)]() mutable {
				// Acknowledged: the faded outputs are at zero from here on.
				h.fade = Fade::out;
				h.fadeAt = 0;
				done();
			});
		}
		void carryLatency(const LatencyPlan& plan) override { harness.plan = plan; }
		void settle() override {}
		std::uint64_t takenAt() const noexcept override { return harness.position; }
		std::uint32_t blockId() const noexcept override { return 7; }
		Impl& harness;
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

	/// An outside node: its companion is marked destroyed with it.
	struct FakeOutsideNode final : OutsideNode {
		FakeOutsideNode(Impl& h, std::size_t i) : harness(h), index(i) {}
		~FakeOutsideNode() override {
			harness.companionList.at(index).destroyed = true;
			harness.outsideEvents.at(index) = nullptr;
		}
		Impl& harness;
		std::size_t index;
	};
	template <typename Request> struct FakeOutside final : OutsideAdapter<Request> {
		explicit FakeOutside(Impl& h) : harness(h) {}
		std::unique_ptr<OutsideNode> make(const OutsideTarget& target, const Request& request,
		                                  OutsideNodeEvents& events) override {
			CompanionFake fake{std::is_same_v<Request, SoundCardRequest> ? CompanionFake::Kind::soundCard
			                                                             : CompanionFake::Kind::rtpReceiver,
			                   request.bank,
			                   target.engineNode,
			                   request,
			                   false,
			                   false,
			                   false,
			                   {}};
			fake.made = true;
			harness.companionList.push_back(std::move(fake));
			harness.outsideEvents.push_back(&events);
			return std::make_unique<FakeOutsideNode>(harness, harness.companionList.size() - 1);
		}
		Impl& harness;
	};

	/// A sink whose consumer runs on the test's thread (docs/writing-a-node-type.md#testing-a-type): begin calls the
	/// consumer's begin at once, then posts `begun`; the tap copies each chunk into its companion and hands it to the
	/// consumer at once; close finishes a consumer that began, marks the companion destroyed and posts `closed(0)`.
	struct FakeSink final : Sink, SinkTap {
		FakeSink(Impl& h, std::size_t i, std::size_t n) : harness(h), index(i), channels(n) {}
		SinkTap& tap() noexcept override { return *this; }
		void begin(std::unique_ptr<SinkConsumer> c, std::move_only_function<void()> begun) override {
			consumer = std::move(c);
			consumer->begin();
			began = true;
			harness.companionList.at(index).ready = true;
			harness.executor.post(std::move(begun));
		}
		void close(std::move_only_function<void(std::uint64_t)> closed) override {
			if (began) consumer->finish();
			harness.companionList.at(index).destroyed = true;
			harness.executor.post([closed = std::move(closed)]() mutable { closed(0); });
		}
		void write(std::span<const std::span<const float>> bank, std::uint32_t frames,
		           const ChunkStamp& stamp) noexcept override {
			SeenChunk seen{std::vector<float>(static_cast<std::size_t>(frames) * channels, 0.0F), frames, stamp};
			for (std::size_t channel = 0; channel < channels && channel < bank.size(); ++channel)
				for (std::uint32_t frame = 0; frame < frames; ++frame)
					seen.interleaved[frame * channels + channel] = bank[channel][frame];
			if (began)
				consumer->consume(Chunk{.interleaved = seen.interleaved,
				                        .frames = frames,
				                        .position = stamp.position,
				                        .startNs = stamp.startNs,
				                        .startRawNs = stamp.startRawNs,
				                        .upstreamFrames = 0,
				                        .discontinuity = stamp.discontinuity});
			harness.companionList.at(index).chunks.push_back(std::move(seen));
		}
		Impl& harness;
		std::size_t index;
		std::size_t channels;
		std::unique_ptr<SinkConsumer> consumer;
		bool began = false;
	};
	struct FakeSinks final : SinkAdapter {
		explicit FakeSinks(Impl& h) : harness(h) {}
		std::unique_ptr<Sink> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
		                           std::uint32_t capacityFrames) override {
			CompanionFake fake{CompanionFake::Kind::sink,
			                   std::string(bank),
			                   node + "." + std::string(bank),
			                   SinkShape{.frames = capacityFrames},
			                   true,
			                   false,
			                   false,
			                   {}};
			harness.companionList.push_back(std::move(fake));
			harness.outsideEvents.push_back(nullptr); // a sink has no outside events
			return std::make_unique<FakeSink>(harness, harness.companionList.size() - 1, channels);
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
		void soundCard(std::string_view bank, SoundCardRequest request) override {
			create.soundCard(bank, std::move(request));
		}
		void rtpReceiver(std::string_view bank, RtpReceiverRequest request) override {
			create.rtpReceiver(bank, std::move(request));
		}
		std::expected<int, std::string> openRecording(const RecordingRequest& request) override {
			harness.recordingList.push_back(request);
			return create.openRecording(request);
		}
		SinkRequest& sink(std::string_view bank, SinkShape shape) override { return create.sink(bank, shape); }
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
			if (const auto* card = std::get_if<SoundCardRequest>(&request)) {
				legacy.acquire(*card);
				return std::monostate{};
			}
			if (const auto* receiver = std::get_if<RtpReceiverRequest>(&request)) {
				legacy.acquire(*receiver);
				return std::monostate{};
			}
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

	enum class Fade { none, in, out };

	Impl(const NodeType& t, Options o)
		: type(t), options(std::move(o)), clock(executor), blocks(*this), cards(*this), receivers(*this),
		  handoffs(*this), sinks(*this),
		  registry(options.recordings != nullptr
	                   ? ResourceRegistry(cards, receivers, handoffs, sinks, *options.recordings)
	                   : ResourceRegistry(cards, receivers, handoffs, sinks)),
		  graph(catalogue), host(std::make_unique<NodeHost>(options.format, blocks, registry, executor, clock)) {
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
		fadeMask.assign(outputNames.size(), false);
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
	FakeOutside<SoundCardRequest> cards;
	FakeOutside<RtpReceiverRequest> receivers;
	FakeHandoffs handoffs;
	FakeSinks sinks;
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
	std::vector<SinkBinding> bound; // the taps the last drive bound (none for a silence)
	LatencyPlan plan;
	std::deque<PendingRequest> pending;
	std::vector<bool> fadeMask;
	Fade fade = Fade::none;
	std::uint32_t fadeAt = 0;
	std::uint64_t position = 0;
	bool firstCycle = true;

	std::vector<CompanionFake> companionList;
	std::vector<OutsideNodeEvents*> outsideEvents; // per companion; null once destroyed
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
	// Last, so it goes first: its nodes' blocks, outside nodes and instances tell the members above as they go.
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
	OutsideNodeEvents* events = impl_->outsideEvents.at(index);
	if (events == nullptr) throw std::logic_error("the outside node on " + std::string(bank) + " is destroyed");
	fake.ready = true;
	events->ready();
	impl_->executor.pump();
}

void NodeHarness::lose(std::string_view bank, std::string detail) {
	CompanionFake& fake = companion(bank);
	const auto index = static_cast<std::size_t>(&fake - impl_->companionList.data());
	OutsideNodeEvents* events = impl_->outsideEvents.at(index);
	if (events == nullptr) throw std::logic_error("the outside node on " + std::string(bank) + " is destroyed");
	events->lost(std::move(detail));
	impl_->executor.pump();
}

void NodeHarness::acknowledgeFade() { impl_->completeOldest(true); }

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
	const auto rate = static_cast<std::int64_t>(h.options.format.rate);
	const auto at = static_cast<std::int64_t>(h.position) * 1'000'000'000 / rate;
	const GraphCycle cycle{.startNs = at,
	                       .position = h.position,
	                       .startRawNs = at,
	                       .nextNs = static_cast<std::int64_t>(h.position + frames) * 1'000'000'000 / rate,
	                       .discontinuity = h.firstCycle};
	if (h.driven != nullptr) h.driven->process(in, out, frames, cycle);
	else
		for (auto& span : out) std::ranges::fill(span, 0.0F);
	// The fade, as the real block applies it (port_blocks.cpp): sample by sample, exact at both ends.
	const std::uint32_t fadeFrames = std::max(h.options.format.rate * 5 / 1000, 1U);
	for (std::uint32_t i = 0; i < frames; ++i) {
		if (h.fade == Impl::Fade::none) break;
		const float gain = static_cast<float>(h.fadeAt) / static_cast<float>(fadeFrames);
		for (std::size_t o = 0; o < out.size(); ++o)
			if (h.fadeMask[o]) {
				out[o][i] = h.fadeAt == 0 ? 0.0F : out[o][i] * gain;
				if (i + 1 == frames) h.lastGain[o] = gain;
			}
		if (h.fade == Impl::Fade::in && ++h.fadeAt >= fadeFrames) {
			h.fade = Impl::Fade::none;
			for (std::size_t o = 0; o < out.size(); ++o)
				if (h.fadeMask[o]) h.lastGain[o] = 1.0F;
		}
	}
	// Each sink's tap after the fade (a sink's bank is never faded), as the real block writes it, while a code runs.
	if (h.driven != nullptr)
		for (const SinkBinding& binding : h.bound) {
			std::vector<std::span<const float>> bank;
			for (std::size_t c = 0; c < binding.channels && binding.firstOutput + c < out.size(); ++c)
				bank.emplace_back(out[binding.firstOutput + c]);
			binding.tap->write(bank, frames,
			                   ChunkStamp{cycle.position, cycle.startNs, cycle.startRawNs, cycle.discontinuity});
		}
	h.position += frames;
	h.firstCycle = false;
	h.lastFrames = frames;
}

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

std::uint64_t NodeHarness::handoffFrames(std::size_t index) const { return impl_->handoffCounts.at(index); }

const std::vector<SeenHandoff>& NodeHarness::handoffs() const noexcept { return impl_->handoffList; }

const std::vector<RecordingRequest>& NodeHarness::recordingRequests() const noexcept { return impl_->recordingList; }

RunObject* NodeHarness::runObject() const noexcept {
	if (impl_->instance == nullptr) return nullptr;
	const auto* legacy = dynamic_cast<const detail::LegacyInstance*>(impl_->instance->inner.get());
	return legacy != nullptr ? legacy->runObject() : nullptr;
}

} // namespace ear::testing
