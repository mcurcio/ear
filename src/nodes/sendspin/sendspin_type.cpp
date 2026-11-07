// The "sendspin" node type (docs/design/sendspin.md sections 3 and 4): a port block with its own audio code and its own
// threads, not an outside node. Its ports are fixed; the connection is the worker's, so an absent server is no
// failure: the node runs and its outputs are silent.
#include "ear/nodes/sendspin.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ear/folder/confined_folder.hpp"
#include "feed.hpp"
#include "key_store.hpp"
#include "port_claims.hpp"
#include "sendspin_code.hpp"
#include "settings.hpp"
#include "worker.hpp"

namespace ear::sendspin {
namespace {

/// Small, so a correction lands within a chunk: the next power of two at or above 40 ms and two blocks (section 7).
std::uint32_t feedFramesFor(const AudioFormat& format) {
	return std::bit_ceil(std::max(format.rate * 40 / 1000, 2 * format.maxBlock));
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

class SendspinInsides final : public NodeInsides {
public:
	SendspinInsides(std::shared_ptr<const folder::ConfinedFolder> folder, const NodeName& node,
	                SendspinSettings settings, NodeGrant& grant, std::optional<HeldPort> port)
		: port_(std::move(port)), feed_(nodeChannels, feedFramesFor(grant.format())),
		  worker_(std::move(folder), node, std::move(settings), grant.format(), feed_, grant.events()),
		  code_(feed_, worker_.progress(), worker_.bell(), grant.format().rate,
	            grant.handoff(nodeChannels, grant.format().rate / 2, worker_.toServer()), worker_.capture()) {}
	AudioCode& audioCode() override { return code_; }
	void close(std::move_only_function<void()> closed) override { worker_.stop(std::move(closed)); }

private:
	std::optional<HeldPort> port_;
	Feed feed_;
	SendspinWorker worker_;
	SendspinCode code_;
};

class SendspinConfig final : public NodeConfig {
public:
	SendspinConfig(std::shared_ptr<const folder::ConfinedFolder> folder, std::shared_ptr<PortClaims> claims,
	               NodeName node, SendspinSettings settings)
		: folder_(std::move(folder)), claims_(std::move(claims)), node_(std::move(node)),
		  settings_(std::move(settings)) {}

	const Ports& ports() const override { return ports_; }

	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
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
		try {
			return std::make_unique<SendspinInsides>(folder_, node_, settings_, grant, std::move(port));
		} catch (const std::system_error& error) {
			return std::unexpected(FailureReason{FailureReason::Code::startFailed, error.what()});
		}
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
	// Boot's one look at the folder: nothing else opens the key file of a node no longer in the graph (section 8).
	for (const std::string& failure : sendspin::narrowKeyFiles(*folders.config))
		std::cerr << "sendspin: " << failure << '\n';
	return std::make_unique<const sendspin::SendspinType>(folders.config);
}

} // namespace ear::nodes
