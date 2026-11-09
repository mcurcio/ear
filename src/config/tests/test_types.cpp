// ear_config's test node types (test_types.hpp). Their tables declare only what the tests need: the
// model reads only the kinds, the plan parses.
#include "test_types.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "ear/core/settings_table.hpp"

namespace ear::config::test {
// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace tables {

class FixedConfig final : public NodeConfig {
public:
	explicit FixedConfig(Ports ports) : ports_(std::move(ports)) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext&) const override {
		return std::unexpected(FailureReason{FailureReason::Code::startFailed, "not started in these tests"});
	}

private:
	Ports ports_;
};

/// Any text at all.
std::optional<std::string> anyText(std::string_view text) { return std::string(text); }

constexpr SettingsTable<> whiteTable{"white"};
constexpr auto pathRow = setting::required("path", setting::text<anyText>("text"));
constexpr SettingsTable<pathRow> fileTable{"file"};
constexpr auto cardRow = setting::optional("card", setting::text<anyText>("text"));
constexpr auto playbackRow = setting::defaulted("playback", setting::number(0, 64), 0);
constexpr auto captureRow = setting::defaulted("capture", setting::number(0, 64), 0);
constexpr SettingsTable<cardRow, playbackRow, captureRow> cardTable{"card"};

class WhiteType final : public NodeTypeOf<whiteTable> {
	MadeConfig make(const NodeName&, const Checked&) const override {
		return std::make_unique<FixedConfig>(Ports{{{Direction::output, "noise", {"mono"}}}});
	}
};

class FileType final : public NodeTypeOf<fileTable> {
	MadeConfig make(const NodeName&, const Checked&) const override {
		return std::make_unique<FixedConfig>(Ports{{{Direction::input, "record", {"left", "right"}}}});
	}
};

class CardType final : public NodeTypeOf<cardTable> {
	MadeConfig make(const NodeName&, const Checked& settings) const override {
		return std::make_unique<FixedConfig>(
			Ports{{{Direction::input, "play", numbered(settings.get<playbackRow>())},
		           {Direction::output, "capture", numbered(settings.get<captureRow>())}}});
	}
};

} // namespace tables
using namespace tables;

std::vector<std::unique_ptr<const NodeType>> testTypes() {
	std::vector<std::unique_ptr<const NodeType>> types;
	types.push_back(std::make_unique<WhiteType>());
	types.push_back(std::make_unique<FileType>());
	types.push_back(std::make_unique<CardType>());
	return types;
}

} // namespace ear::config::test
