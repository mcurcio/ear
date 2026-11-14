// The "sendspin" node type (docs/node-types.md#sendspin): a port block with its own audio code and its own
// threads, not an engine companion. Its ports are fixed; the connection is the worker's, so an absent server is no
// failure: the node runs and its outputs are silent.
#include "ear/nodes/sendspin.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ear/core/log.hpp"
#include "ear/folder/confined_folder.hpp"
#include "feed.hpp"
#include "key_store.hpp"
#include "port_claims.hpp"
#include "sendspin_code.hpp"
#include "settings.hpp"
#include "worker.hpp"

namespace ear::sendspin {
namespace {

/// Small, so a correction lands within a chunk: the next power of two at or above 40 ms and two blocks.
std::uint32_t feedFramesFor(const AudioFormat& format) {
	const auto forty = static_cast<std::uint32_t>(FrameClock(format.rate).frames(std::chrono::milliseconds{40}));
	return std::bit_ceil(std::max(forty, 2 * format.maxBlock));
}

/// A listening node's claim on its port, for as long as its insides live, so past the library's own socket.
class HeldPort {
public:
	HeldPort(std::shared_ptr<PortClaims> claims, std::uint16_t port) : claims_(std::move(claims)), port_(port) {}
	HeldPort(HeldPort&& other) noexcept : claims_(std::move(other.claims_)), port_(other.port_) {}
	~HeldPort() {
		if (claims_) claims_->release(port_);
	}

private:
	std::shared_ptr<PortClaims> claims_;
	std::uint16_t port_;
};

/// The run object: the claim, the feed and the worker with its threads. Until slice 3c (docs/design/node-primitives.md)
/// moves sendspin onto the framework's loops, feed, stores and claims, it is built in start (the worker reads the key
/// file and starts its threads there), and its recipe only hands it over.
class SendspinRun final : public RunObject {
public:
	SendspinRun(std::shared_ptr<const folder::ConfinedFolder> folder, const NodeName& node, SendspinSettings settings,
	            StartContext& context, std::optional<HeldPort> port)
		: port_(std::move(port)), feed_(nodeChannels, feedFramesFor(context.format())),
		  worker_(std::move(folder), node, std::move(settings), context.format(), feed_, context.events()) {}
	void close(std::move_only_function<void()> closed) override { worker_.stop(std::move(closed)); }
	/// The run's code, reading this run's feed and writing `toServer`; it must not outlive the run.
	std::unique_ptr<SendspinCode> code(SampleRate rate, RtWriter& toServer) {
		return std::make_unique<SendspinCode>(feed_, worker_.progress(), worker_.bell(), rate, toServer,
		                                      worker_.capture());
	}
	HandoffConsumer& toServer() noexcept { return worker_.toServer(); }

private:
	std::optional<HeldPort> port_;
	Feed feed_;
	SendspinWorker worker_;
};

class SendspinConfig final : public NodeConfig {
public:
	SendspinConfig(std::shared_ptr<const folder::ConfinedFolder> folder, std::shared_ptr<PortClaims> claims,
	               NodeName node, SendspinSettings settings)
		: folder_(std::move(folder)), claims_(std::move(claims)), node_(std::move(node)),
		  settings_(std::move(settings)) {}

	const Ports& ports() const override { return ports_; }

	StartResult start(StartContext& context) const override {
		std::optional<HeldPort> port;
		if (!settings_.server) {
			const auto refused = [&](const std::string& reason) {
				return std::unexpected(
					FailureReason{FailureReason::Code::startFailed,
				                  "cannot listen on port " + std::to_string(settings_.port) + ": " + reason});
			};
			if (auto claimed = claims_->claim(settings_.port, node_); !claimed) return refused(claimed.error());
			port.emplace(claims_, settings_.port);
			if (auto probed = probeListenPort(settings_.port); !probed) return refused(probed.error());
		}
		std::unique_ptr<SendspinRun> run;
		try {
			run = std::make_unique<SendspinRun>(folder_, node_, settings_, context, std::move(port));
		} catch (const std::system_error& error) {
			return std::unexpected(FailureReason{FailureReason::Code::startFailed, error.what()});
		}
		const SampleRate rate = context.format().rate;
		// The handoff to the server holds half a second.
		const auto half = static_cast<std::uint32_t>(FrameClock(rate).frames(std::chrono::milliseconds{500}));
		RtWriter& toServer = context.acquire(HandoffRequest{"to", half, &run->toServer()});
		// The feed holds the server's audio up to its size before it leaves (docs/engine.md#latency-per-bank-pair).
		context.declareLatency("from", Latency{0, feedFramesFor(context.format())});
		auto code = run->code(rate, toServer);
		return StartResult{
			std::move(code),
			[run = std::move(run)](Ready&) mutable -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
				return std::move(run);
			}};
	}

private:
	std::shared_ptr<const folder::ConfinedFolder> folder_;
	std::shared_ptr<PortClaims> claims_;
	NodeName node_;
	SendspinSettings settings_;
	Ports ports_{{{Direction::input, "to", {"left", "right"}}, {Direction::output, "from", {"left", "right"}}}};
};

class SendspinType final : public NodeTypeOf<settingsTable> {
public:
	explicit SendspinType(std::shared_ptr<const folder::ConfinedFolder> folder) : folder_(std::move(folder)) {}

private:
	MadeConfig make(const NodeName& node, const Checked& settings) const override {
		return std::make_unique<const SendspinConfig>(folder_, claims_, node, settingsFrom(node, settings));
	}

	std::shared_ptr<const folder::ConfinedFolder> folder_; // the graph file folder: each node's key file
	std::shared_ptr<PortClaims> claims_ = std::make_shared<PortClaims>();
};

} // namespace
} // namespace ear::sendspin

namespace ear::nodes {

std::unique_ptr<const NodeType> makeSendspinNodeType(const NodeFolders& folders) {
	if (!folders.config) throw std::logic_error("sendspin needs the graph file folder");
	// Boot's one look at the folder: nothing else opens the key file of a node no longer in the graph.
	if (folders.log == nullptr) throw std::logic_error("sendspin needs the boot log");
	for (std::string& failure : sendspin::narrowKeyFiles(*folders.config))
		folders.log->log({.level = LogLevel::warn, .source = "sendspin", .text = std::move(failure)});
	return std::make_unique<const sendspin::SendspinType>(folders.config);
}

} // namespace ear::nodes
