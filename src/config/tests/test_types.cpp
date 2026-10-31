// ear_config's test node types (test_types.hpp). Their parse checks only what the tests need: the
// model never calls it, the plan does.
#include "test_types.hpp"

#include <array>
#include <string>

namespace ear::config::test {
namespace {

class FixedConfig final : public NodeConfig {
public:
	explicit FixedConfig(Ports ports) : ports_(std::move(ports)) {}
	const Ports& ports() const override { return ports_; }
	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant&) const override {
		return std::unexpected(FailureReason{FailureReason::Code::startFailed, "not started in these tests"});
	}

private:
	Ports ports_;
};

class WhiteType final : public NodeType {
public:
	std::string_view name() const override { return "white"; }
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName& node,
	                                                                        const NodeSettings& settings) const override {
		if (!settings.empty()) return std::unexpected(InvalidSettings{node, "white takes no settings"});
		return std::make_unique<FixedConfig>(Ports{{}, {"MONO"}});
	}
};

class FileType final : public NodeType {
public:
	std::string_view name() const override { return "file"; }
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName& node,
	                                                                        const NodeSettings& settings) const override {
		if (!settings.contains("path") || !settings["path"].is_string())
			return std::unexpected(InvalidSettings{node, "file needs a path"});
		return std::make_unique<FixedConfig>(Ports{{"FL", "FR"}, {}});
	}
	std::span<const SettingDeclaration> settingKinds() const override { return kinds; }

private:
	static constexpr std::array<SettingDeclaration, 1> kinds{{{"path", SettingKind::text}}};
};

class CardType final : public NodeType {
public:
	std::string_view name() const override { return "card"; }
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName&,
	                                                                        const NodeSettings& settings) const override {
		Ports ports;
		const auto playback = settings.value("playback", 0);
		for (int i = 1; i <= playback; ++i) ports.inputs.push_back("playback_" + std::to_string(i));
		const auto capture = settings.value("capture", 0);
		for (int i = 1; i <= capture; ++i) ports.outputs.push_back("capture_" + std::to_string(i));
		return std::make_unique<FixedConfig>(std::move(ports));
	}
	std::span<const SettingDeclaration> settingKinds() const override { return kinds; }

private:
	static constexpr std::array<SettingDeclaration, 3> kinds{{{"card", SettingKind::text},
	                                                          {"playback", SettingKind::wholeNumber},
	                                                          {"capture", SettingKind::wholeNumber}}};
};

} // namespace

std::vector<std::unique_ptr<const NodeType>> testTypes() {
	std::vector<std::unique_ptr<const NodeType>> types;
	types.push_back(std::make_unique<WhiteType>());
	types.push_back(std::make_unique<FileType>());
	types.push_back(std::make_unique<CardType>());
	return types;
}

} // namespace ear::config::test
