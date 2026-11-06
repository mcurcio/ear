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
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "feed.hpp"
#include "sendspin_code.hpp"
#include "settings.hpp"
#include "worker.hpp"

namespace ear::sendspin {
namespace {

constexpr std::string_view typeName = "sendspin";
constexpr std::array<SettingDeclaration, 2> settingDeclarations{{{"server", SettingKind::text}, {"name", SettingKind::text}}};
constexpr std::uint32_t channelCount = 2;

/// Small, so a correction lands within a chunk: the next power of two at or above 40 ms and two blocks (section 7).
std::uint32_t feedFramesFor(const AudioFormat& format) {
	return std::bit_ceil(std::max(format.rate * 40 / 1000, 2 * format.maxBlock));
}

class SendspinInsides final : public NodeInsides {
public:
	SendspinInsides(std::shared_ptr<const folder::ConfinedFolder> folder, const NodeName& node, SendspinSettings settings,
	                NodeGrant& grant)
	    : feed_(channelCount, feedFramesFor(grant.format())),
	      worker_(std::move(folder), node, std::move(settings), grant.format(), feed_, grant.events(), feed_.capacityFrames()),
	      code_(feed_, worker_.progress(), worker_.bell(), grant.format().rate,
	            grant.handoff(channelCount, grant.format().rate / 2, worker_.toServer()), worker_.capture()) {}
	AudioCode& audioCode() override { return code_; }
	void close(std::move_only_function<void()> closed) override { worker_.stop(std::move(closed)); }

private:
	Feed feed_;
	SendspinWorker worker_;
	SendspinCode code_;
};

class SendspinConfig final : public NodeConfig {
public:
	SendspinConfig(std::shared_ptr<const folder::ConfinedFolder> folder, NodeName node, SendspinSettings settings)
	    : folder_(std::move(folder)), node_(std::move(node)), settings_(std::move(settings)) {}

	const Ports& ports() const override { return ports_; }

	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		try {
			return std::make_unique<SendspinInsides>(folder_, node_, settings_, grant);
		} catch (const std::system_error& error) {
			return std::unexpected(FailureReason{FailureReason::Code::startFailed, error.what()});
		}
	}

private:
	std::shared_ptr<const folder::ConfinedFolder> folder_;
	NodeName node_;
	SendspinSettings settings_;
	Ports ports_{.inputs = {"to_server_FL", "to_server_FR"}, .outputs = {"from_server_FL", "from_server_FR"}};
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
		return std::make_unique<const SendspinConfig>(folder_, node, std::move(*parsed));
	}

private:
	std::shared_ptr<const folder::ConfinedFolder> folder_;   // the graph file folder: each node's key file
};

} // namespace

std::unique_ptr<const NodeType> makeSendspinNodeType(std::shared_ptr<const folder::ConfinedFolder> folder) {
	return std::make_unique<const SendspinType>(std::move(folder));
}

} // namespace ear::sendspin
