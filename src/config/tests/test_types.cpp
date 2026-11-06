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
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
	parse(const NodeName& node, const NodeSettings& settings) const override {
		if (!settings.empty()) return std::unexpected(InvalidSettings{node, "white takes no settings"});
		return std::make_unique<FixedConfig>(Ports{{{Direction::output, "noise", {"mono"}}}});
	}
	std::span<const SettingDeclaration> settingKinds() const override { return {}; }
};

class FileType final : public NodeType {
public:
	std::string_view name() const override { return "file"; }
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
	parse(const NodeName& node, const NodeSettings& settings) const override {
		if (!settings.contains("path") || !settings["path"].is_string())
			return std::unexpected(InvalidSettings{node, "file needs a path"});
		return std::make_unique<FixedConfig>(Ports{{{Direction::input, "record", {"left", "right"}}}});
	}
	std::span<const SettingDeclaration> settingKinds() const override { return kinds; }

private:
	static constexpr std::array<SettingDeclaration, 1> kinds{{{"path", SettingKind::text}}};
};

class CardType final : public NodeType {
public:
	std::string_view name() const override { return "card"; }
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
	parse(const NodeName&, const NodeSettings& settings) const override {
		const auto channels = [&](const char* key) {
			return numbered(static_cast<std::uint32_t>(settings.value(key, 0)));
		};
		return std::make_unique<FixedConfig>(Ports{
			{{Direction::input, "play", channels("playback")}, {Direction::output, "capture", channels("capture")}}});
	}
	std::span<const SettingDeclaration> settingKinds() const override { return kinds; }

private:
	static constexpr std::array<SettingDeclaration, 3> kinds{
		{{"card", SettingKind::text}, {"playback", SettingKind::wholeNumber}, {"capture", SettingKind::wholeNumber}}};
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
