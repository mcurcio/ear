// ear_config's test node types (test_types.hpp). Their tables declare only what the tests need: the
// model reads only the kinds, the plan parses.
#include "test_types.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "ear/core/lifecycle.hpp"

namespace ear::config::test {
// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace tables {

/// What every type here answers at configure: these tests read, plan and parse, and never configure a node.
struct NeverStarted {
	template <class Create, class Checked> static Configured never(Create&, const Checked&) {
		return Configured::failed("not started in these tests");
	}
};

/// Any text at all.
std::optional<std::string> anyText(std::string_view text) { return std::string(text); }

constexpr auto noise = bank::output<"noise">;
constexpr auto record = bank::input<"record">;
constexpr auto play = bank::input<"play">;
constexpr auto capture = bank::output<"capture">;
constexpr auto stream = bank::output<"stream">;

constexpr TypeSpec whiteSpec{.settings = SettingsTable<>{"white"}, .banks = Banks<noise>{}};
struct WhiteType : NeverStarted {
	static Members<whiteSpec> ports(const CheckedOf<whiteSpec>&) { return {noise(members("mono"))}; }
	Configured configure(CreateOf<whiteSpec>& cx, const CheckedOf<whiteSpec>& s) { return never(cx, s); }
};

constexpr auto pathRow = setting::required("path", setting::text<anyText>("text"));
constexpr TypeSpec fileSpec{.settings = SettingsTable<pathRow>{"file"}, .banks = Banks<record>{}};
struct FileType : NeverStarted {
	static Members<fileSpec> ports(const CheckedOf<fileSpec>&) { return {record(members("left", "right"))}; }
	Configured configure(CreateOf<fileSpec>& cx, const CheckedOf<fileSpec>& s) { return never(cx, s); }
};

constexpr auto cardRow = setting::optional("card", setting::text<anyText>("text"));
constexpr auto playbackRow = setting::defaulted("playback", setting::number(0, 64), 0);
constexpr auto captureRow = setting::defaulted("capture", setting::number(0, 64), 0);
constexpr TypeSpec cardSpec{.settings = SettingsTable<cardRow, playbackRow, captureRow>{"card"},
                            .banks = Banks<play, capture>{}};
struct CardType : NeverStarted {
	static Members<cardSpec> ports(const CheckedOf<cardSpec>& settings) {
		return {play(members(settings.get<playbackRow>())), capture(members(settings.get<captureRow>()))};
	}
	Configured configure(CreateOf<cardSpec>& cx, const CheckedOf<cardSpec>& s) { return never(cx, s); }
};

constexpr auto rateRow =
	setting::optional("rate", setting::rate(SampleRate::fixed<8'000>(), SampleRate::fixed<192'000>()));
constexpr TypeSpec rtpSpec{.settings = SettingsTable<rateRow>{"rtp"}, .banks = Banks<stream>{}};
struct RtpType : NeverStarted {
	static Members<rtpSpec> ports(const CheckedOf<rtpSpec>&) { return {stream(members(1))}; }
	Configured configure(CreateOf<rtpSpec>& cx, const CheckedOf<rtpSpec>& s) { return never(cx, s); }
};

constexpr auto xRow = setting::optional("x", setting::boolean());
constexpr TypeSpec lampSpec{.settings = SettingsTable<xRow>{"lamp"}, .banks = Banks<>{}};
struct LampType : NeverStarted {
	static Members<lampSpec> ports(const CheckedOf<lampSpec>&) { return {}; }
	Configured configure(CreateOf<lampSpec>& cx, const CheckedOf<lampSpec>& s) { return never(cx, s); }
};

} // namespace tables
using namespace tables;

std::vector<std::unique_ptr<const NodeType>> testTypes() {
	std::vector<std::unique_ptr<const NodeType>> types;
	types.push_back(makeNodeType<whiteSpec, WhiteType>());
	types.push_back(makeNodeType<fileSpec, FileType>());
	types.push_back(makeNodeType<cardSpec, CardType>());
	types.push_back(makeNodeType<rtpSpec, RtpType>());
	types.push_back(makeNodeType<lampSpec, LampType>());
	return types;
}

} // namespace ear::config::test
