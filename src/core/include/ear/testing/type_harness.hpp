// A type's own tests run it through this (docs/writing-a-node-type.md#testing-a-type): the NodeHarness of the type
// makeNodeType<Spec, Type>() makes, over a real NodeHost, with banks named by the spec's handles, so a test names banks
// as the type does and a bank the spec lacks does not compile.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "ear/core/lifecycle.hpp"
#include "ear/testing/node_harness.hpp"
namespace ear::testing {

template <const auto& Spec, class Type> class TypeHarness {
	template <class Bank> static constexpr bool ownBank = isBankHandle<Bank> && BanksOf<Spec>::template has<Bank>;

public:
	using Options = HarnessOptions;
	explicit TypeHarness(Options options = {})
		: type_(makeNodeType<Spec, Type>()), node_(std::make_unique<NodeHarness>(*type_, std::move(options))) {}
	const NodeType& type() const noexcept { return *type_; }
	NodeHarness& node() noexcept { return *node_; }
	NodeHarness* operator->() noexcept { return node_.get(); }

	std::optional<std::string> parse(const NodeSettings& settings) { return node_->parse(settings); }
	void add(AddOptions options = {}) { node_->add(options); }
	void blockReady() { node_->blockReady(); }
	void process(std::uint32_t frames = 0) { node_->process(frames); }
	NodeOutcome change(const NodeSettings& settings) { return node_->change(settings); }
	LifecycleReply stop() { return node_->stop(); }
	LifecycleReply start() { return node_->start(); }
	LifecycleReply restart() { return node_->restart(); }
	void disable() { node_->disable(); }
	void enable() { node_->enable(); }
	void remove() { node_->remove(); }
	std::string_view phase() const { return node_->phase(); }
	NodeHealth health() const { return node_->health(); }
	bool gone() const noexcept { return node_->gone(); }
	std::uint64_t droppedFrames() const { return node_->droppedFrames(); }
	std::size_t unwritten() const { return node_->unwritten(); }
	const std::vector<CompanionFake>& companions() const noexcept { return node_->companions(); }
	const std::vector<SeenHandoff>& handoffs() const noexcept { return node_->handoffs(); }
	const std::vector<RecordingRequest>& recordingRequests() const noexcept { return node_->recordingRequests(); }

	template <class Bank> void ready(Bank) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		node_->ready(Bank::name);
	}
	template <class Bank> void lose(Bank, std::string detail) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		node_->lose(Bank::name, std::move(detail));
	}
	template <class Bank> std::span<float> input(Bank, std::size_t member) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::input, "input() takes an input bank");
		return node_->input(node_->ports().bank(Bank::name).ports.at(member));
	}
	template <class Bank> std::span<const float> output(Bank, std::size_t member) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::output, "output() takes an output bank");
		return node_->output(node_->ports().bank(Bank::name).ports.at(member));
	}
	template <class Bank> CompanionFake& companion(Bank) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		return node_->companion(Bank::name);
	}
	template <class Bank> float lastFadeGain(Bank, std::size_t member = 0) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::output, "a fade is on an output bank");
		return node_->lastFadeGain(node_->ports().bank(Bank::name).ports.at(member));
	}

private:
	std::unique_ptr<const NodeType> type_;
	std::unique_ptr<NodeHarness> node_; // after the type it runs
};

} // namespace ear::testing
