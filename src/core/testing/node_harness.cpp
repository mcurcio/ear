// The node harness (ear/testing/node_harness.hpp): NodeHost's run, on the test's one thread, with fake resources.
#include "ear/testing/node_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ear/core/pass_through.hpp"
#include "ear/core/run_requests.hpp"

namespace ear::testing {

/// A handoff whose writes go straight to its consumer, on the writing thread (the test's).
struct NodeHarness::Handoff final : RtWriter {
	Handoff(HandoffConsumer& consumer, std::size_t channels) : consumer(&consumer), channels(channels) {}
	std::uint32_t write(std::span<const std::span<const float>> planar, std::uint32_t count) noexcept override {
		interleaved.assign(static_cast<std::size_t>(count) * channels, 0.0F);
		for (std::size_t channel = 0; channel < channels && channel < planar.size(); ++channel)
			for (std::uint32_t frame = 0; frame < count; ++frame)
				interleaved[frame * channels + channel] = planar[channel][frame];
		consumer->consume(interleaved, count);
		frames += count;
		return count;
	}
	HandoffConsumer* consumer;
	std::size_t channels;
	std::vector<float> interleaved;
	std::uint64_t frames = 0;
};

/// The run's events, from any thread: held under a lock, read by the test.
class NodeHarness::Events final : public InsidesEvents {
public:
	void say(std::string text) override {
		const std::scoped_lock lock(mutex);
		if (!detached) notes.push_back(std::move(text));
	}
	void lost(std::string detail) override {
		const std::scoped_lock lock(mutex);
		if (!detached && !closing && !lostDetail) lostDetail = std::move(detail);
	}
	mutable std::mutex mutex;
	std::condition_variable closedSignal;
	std::vector<std::string> notes;
	std::optional<std::string> lostDetail;
	bool closing = false;
	bool detached = false;
	bool closed = false;
};

NodeHarness::NodeHarness(const NodeType& type, AudioFormat format, RecordingAdapter* recordings, NodeName node)
	: type_(type), format_(format), recordings_(recordings), node_(std::move(node)) {}

NodeHarness::~NodeHarness() {
	if (state_ == State::starting || state_ == State::running || state_ == State::failed) {
		try {
			close();
		} catch (...) { // NOLINT(bugprone-empty-catch): a destructor reports nothing; the test's own close() would
		}
	}
}

std::expected<void, std::string> NodeHarness::parse(const NodeSettings& settings) {
	if (state_ != State::unparsed && state_ != State::parsed)
		throw std::logic_error("the harness parses before its run starts");
	auto parsed = type_.parse(node_, settings);
	if (!parsed) {
		config_.reset();
		state_ = State::unparsed;
		return std::unexpected(std::move(parsed.error().detail));
	}
	config_ = std::move(*parsed);
	state_ = State::parsed;
	const Ports& ports = config_->ports();
	inputs_.assign(ports.inputs().size() + ports.insideInputs().size(), std::vector<float>(format_.maxBlock, 0.0F));
	outputs_.assign(ports.outputs().size() + ports.insideOutputs().size(),
	                std::vector<float>(format_.maxBlock, std::numeric_limits<float>::quiet_NaN()));
	return {};
}

const Ports& NodeHarness::ports() const {
	if (!config_) throw std::logic_error("the harness has no parsed node");
	return config_->ports();
}

std::expected<void, FailureReason> NodeHarness::start() {
	if (state_ != State::parsed) throw std::logic_error("the harness starts once, after a parse that passed");
	state_ = State::starting;
	events_ = std::make_shared<Events>();
	inStart_ = true;
	StartResult started = [&] {
		try {
			return config_->start(*this);
		} catch (...) {
			inStart_ = false;
			throw;
		}
	}();
	inStart_ = false;
	if (started.failed()) {
		handoffs_.clear();
		recordingFiles_.clear();
		outside_.clear();
		fail(started.failure());
		return std::unexpected(started.failure());
	}
	recipe_ = std::move(started.recipe());
	code_ = started.code().own() ? std::move(started.code().own()) : makePassThrough(ports(), started.code().pairs());
	code_->prepare(format_);
	buildIfAllReady();
	if (state_ == State::failed) return std::unexpected(*failure_);
	return {};
}

void NodeHarness::ready(std::size_t index) {
	outside_.at(index).ready = true;
	buildIfAllReady();
}

void NodeHarness::lose(std::size_t index, std::string detail) {
	static_cast<void>(outside_.at(index));
	fail({FailureReason::Code::engineRefused, std::move(detail)});
}

NodeHarness::State NodeHarness::state() const {
	const_cast<NodeHarness*>(this)->applyFacts(); // NOLINT: a fact told from a run's thread lands when looked at
	return state_;
}

std::optional<FailureReason> NodeHarness::failure() const {
	const_cast<NodeHarness*>(this)->applyFacts(); // NOLINT: as state()
	return failure_;
}

std::span<float> NodeHarness::input(std::string_view port) {
	const Ports& known = ports();
	const auto at = [&](const std::vector<PortName>& names) { return std::ranges::find(names, port) - names.begin(); };
	if (const auto index = static_cast<std::size_t>(at(known.inputs())); index < known.inputs().size())
		return inputs_[index];
	if (const auto index = static_cast<std::size_t>(at(known.insideInputs())); index < known.insideInputs().size())
		return inputs_[known.inputs().size() + index];
	throw std::logic_error("node " + node_ + " has no input " + std::string(port));
}

std::span<const float> NodeHarness::output(std::string_view port) const {
	const Ports& known = ports();
	const auto at = [&](const std::vector<PortName>& names) { return std::ranges::find(names, port) - names.begin(); };
	if (const auto index = static_cast<std::size_t>(at(known.outputs())); index < known.outputs().size())
		return outputs_[index];
	if (const auto index = static_cast<std::size_t>(at(known.insideOutputs())); index < known.insideOutputs().size())
		return outputs_[known.outputs().size() + index];
	throw std::logic_error("node " + node_ + " has no output " + std::string(port));
}

void NodeHarness::process(const GraphCycle& cycle, std::uint32_t frames) {
	if (!code_) throw std::logic_error("the harness processes only a started run");
	if (frames > format_.maxBlock) throw std::logic_error("a cycle is at most maxBlock frames");
	std::vector<std::span<const float>> in;
	for (const auto& buffer : inputs_) in.emplace_back(std::span<const float>(buffer).first(frames));
	std::vector<std::span<float>> out;
	for (auto& buffer : outputs_) {
		std::ranges::fill(buffer, std::numeric_limits<float>::quiet_NaN());
		out.emplace_back(std::span<float>(buffer).first(frames));
	}
	code_->process(in, out, frames, cycle);
	lastFrames_ = frames;
}

std::size_t NodeHarness::unwritten() const {
	std::size_t count = 0;
	for (const auto& buffer : outputs_)
		count += static_cast<std::size_t>(std::ranges::count_if(std::span<const float>(buffer).first(lastFrames_),
		                                                        [](float sample) { return std::isnan(sample); }));
	return count;
}

std::uint64_t NodeHarness::handoffFrames(std::size_t index) const { return handoffs_.at(index)->frames; }

std::vector<std::string> NodeHarness::notes() const {
	if (!events_) return {};
	const std::scoped_lock lock(events_->mutex);
	return events_->notes;
}

void NodeHarness::close() {
	if (state_ != State::starting && state_ != State::running && state_ != State::failed)
		throw std::logic_error("the harness closes only a started run");
	{
		const std::scoped_lock lock(events_->mutex);
		events_->closing = true;
	}
	if (code_) code_->release();
	outside_.clear();
	for (const auto& handoff : handoffs_) handoff->consumer->finish();
	handoffs_.clear();
	if (object_) {
		std::weak_ptr<Events> weak = events_;
		object_->close([weak] {
			if (const auto events = weak.lock()) {
				const std::scoped_lock lock(events->mutex);
				events->closed = true;
				events->closedSignal.notify_all();
			}
		});
		std::unique_lock lock(events_->mutex);
		if (!events_->closedSignal.wait_for(lock, std::chrono::seconds(10), [&] { return events_->closed; }))
			throw std::runtime_error("node " + node_ + ": the run object did not close within 10 s");
	}
	{
		const std::scoped_lock lock(events_->mutex);
		events_->detached = true;
	}
	code_.reset();
	object_.reset();
	recipe_ = nullptr;
	recordingFiles_.clear();
	state_ = State::closed;
}

InsidesEvents& NodeHarness::events() {
	if (!inStart_ && !inRecipe_)
		throw std::logic_error("node " + node_ + ": events were asked for outside start() and its recipe");
	return *events_;
}

void NodeHarness::declareLatency(const std::string& bank, Latency internal) {
	if (!inStart_) throw std::logic_error("node " + node_ + ": latency was declared outside start()");
	static_cast<void>(ports().bank(bank)); // throws for a bank the node lacks, as NodeHost does
	if (std::ranges::any_of(latency_, [&](const auto& declared) { return declared.first == bank; }))
		throw std::logic_error("node " + node_ + ": latency declared twice for bank \"" + bank + "\"");
	latency_.emplace_back(bank, internal);
}

StartContext::Acquired NodeHarness::acquireResource(const ResourceRequest& request) {
	if (!inStart_) throw std::logic_error("node " + node_ + ": a resource was asked for outside start()");
	requests_.push_back(request);
	// NodeHost's own checks and names (run_requests.hpp), so the harness accepts what NodeHost accepts.
	const Ports::Bank* bank = requestedBank(ports(), node_, request);
	if (const auto suffix = outsideSuffix(request)) {
		NodeName name = node_ + "." + std::string(*suffix);
		if (std::ranges::any_of(outside_, [&](const Outside& known) { return known.name == name; }))
			throw std::logic_error("node " + node_ + ": a second resource named " + name + " in one run");
		outside_.push_back({std::move(name)});
		return std::monostate{};
	}
	if (const auto* handoff = std::get_if<HandoffRequest>(&request)) {
		RtWriter* writer =
			handoffs_.emplace_back(std::make_unique<Handoff>(*handoff->consumer, bank->ports.size())).get();
		return writer;
	}
	const auto& recording = std::get<RecordingRequest>(request);
	if (recordings_ == nullptr) return std::unexpected(std::string("no recordings folder"));
	auto opened = recordings_->open(node_, recording.path);
	if (!opened) return std::unexpected(std::move(opened.error()));
	return (*recordingFiles_.emplace_back(std::move(*opened))).descriptor();
}

void NodeHarness::buildIfAllReady() {
	applyFacts();
	if (state_ != State::starting || !std::ranges::all_of(outside_, &Outside::ready)) return;
	if (recipe_) {
		inRecipe_ = true;
		auto made = [&] {
			try {
				return recipe_(*this);
			} catch (...) {
				inRecipe_ = false;
				throw;
			}
		}();
		inRecipe_ = false;
		if (!made) {
			fail(std::move(made.error()));
			return;
		}
		object_ = std::move(*made);
	}
	state_ = State::running;
}

void NodeHarness::fail(FailureReason reason) {
	if (state_ == State::failed || state_ == State::closed) return;
	failure_ = std::move(reason);
	state_ = State::failed;
}

void NodeHarness::applyFacts() {
	if (!events_ || (state_ != State::starting && state_ != State::running)) return;
	std::optional<std::string> lost;
	{
		const std::scoped_lock lock(events_->mutex);
		lost = std::exchange(events_->lostDetail, std::nullopt);
	}
	if (lost) fail({FailureReason::Code::startFailed, std::move(*lost)});
}

} // namespace ear::testing
