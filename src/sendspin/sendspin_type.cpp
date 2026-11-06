// The "sendspin" node type (docs/design/sendspin.md sections 3 and 4): a port block with its own audio code and its own
// threads, not an outside node. Its ports are fixed; the connection is the worker's, so an absent server is no
// failure: the node runs and its outputs are silent.
#include "ear/sendspin/sendspin_type.hpp"

#include <algorithm>
#include <array>
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

#include "feed.hpp"
#include "key_store.hpp"
#include "port_claims.hpp"
#include "sendspin_code.hpp"
#include "settings.hpp"
#include "worker.hpp"

namespace ear::sendspin {
namespace {

constexpr std::string_view typeName = "sendspin";
constexpr std::array<SettingDeclaration, 3> settingDeclarations{{{"server", SettingKind::text},
                                                                 {"port", SettingKind::wholeNumber},
                                                                 {"name", SettingKind::text}}};

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
	SendspinInsides(std::shared_ptr<const folder::ConfinedFolder> folder, const NodeName& node, SendspinSettings settings,
	                NodeGrant& grant, std::optional<HeldPort> port)
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
	SendspinConfig(std::shared_ptr<const folder::ConfinedFolder> folder, std::shared_ptr<PortClaims> claims, NodeName node, SendspinSettings settings)
	    : folder_(std::move(folder)), claims_(std::move(claims)), node_(std::move(node)), settings_(std::move(settings)) {}

	const Ports& ports() const override { return ports_; }

	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		std::optional<HeldPort> port;
		if (!settings_.server) {
			const auto refused = [&](const std::string& reason) {
				return std::unexpected(FailureReason{FailureReason::Code::startFailed,
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

class SendspinType final : public NodeType {
public:
	explicit SendspinType(std::shared_ptr<const folder::ConfinedFolder> folder) : folder_(std::move(folder)) {}

	std::string_view name() const override { return typeName; }

	std::span<const SettingDeclaration> settingKinds() const override { return settingDeclarations; }

	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
	parse(const NodeName& node, const NodeSettings& settings) const override {
		auto parsed = parseSettings(node, settings);
		if (!parsed) return std::unexpected(InvalidSettings{node, std::move(parsed.error())});
		return std::make_unique<const SendspinConfig>(folder_, claims_, node, std::move(*parsed));
	}

private:
	std::shared_ptr<const folder::ConfinedFolder> folder_;   // the graph file folder: each node's key file
	std::shared_ptr<PortClaims> claims_ = std::make_shared<PortClaims>();
};

} // namespace

std::unique_ptr<const NodeType> makeSendspinNodeType(std::shared_ptr<const folder::ConfinedFolder> folder) {
	// Boot's one look at the folder: nothing else opens the key file of a node no longer in the graph (section 8).
	if (folder) {
		for (const std::string& failure : narrowKeyFiles(*folder)) std::cerr << "sendspin: " << failure << '\n';
	}
	return std::make_unique<const SendspinType>(std::move(folder));
}

} // namespace ear::sendspin
