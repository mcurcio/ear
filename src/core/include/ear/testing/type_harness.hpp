// A type's own tests run it through this (docs/design/node-framework.md section 7.6): the NodeHarness of the type
// makeNodeType<Spec, Behaviour>() makes, plus its buffers by the spec's bank handles, so a test names banks as the type
// does and a bank the spec lacks does not compile.
#pragma once
#include <cstddef>
#include <memory>
#include <span>
#include "ear/core/type_spec.hpp"
#include "ear/testing/node_harness.hpp"
namespace ear::testing {
template <const auto& Spec, class Behaviour> class TypeHarness {
public:
	/// `recordings` (null: every recording is refused) outlives the harness.
	explicit TypeHarness(AudioFormat format = {.rate = 48000, .maxBlock = 256}, RecordingAdapter* recordings = nullptr,
	                     NodeName node = "node")
		: type_(makeNodeType<Spec, Behaviour>()),
		  node_(std::make_unique<NodeHarness>(*type_, format, recordings, std::move(node))) {}
	const NodeType& type() const noexcept { return *type_; }
	NodeHarness& node() noexcept { return *node_; }
	NodeHarness* operator->() noexcept { return node_.get(); }
	/// Member `member` of an input bank of the spec: to fill before process().
	template <class Handle> std::span<float> input(Handle, std::size_t member) {
		static_assert(BanksOf<Spec>::template has<Handle>, "the harness takes only its spec's banks");
		static_assert(Handle::direction == Direction::input, "input() takes an input bank");
		return node_->input(node_->ports().bank(Handle::name).ports.at(member));
	}
	/// Member `member` of an output bank of the spec: what the last process() wrote.
	template <class Handle> std::span<const float> output(Handle, std::size_t member) const {
		static_assert(BanksOf<Spec>::template has<Handle>, "the harness takes only its spec's banks");
		static_assert(Handle::direction == Direction::output, "output() takes an output bank");
		return node_->output(node_->ports().bank(Handle::name).ports.at(member));
	}

private:
	std::unique_ptr<const NodeType> type_;
	std::unique_ptr<NodeHarness> node_; // after the type it runs
};
} // namespace ear::testing
