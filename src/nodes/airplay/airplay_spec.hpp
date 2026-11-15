// The "airplay" node type's spec and class (docs/node-types.md#airplay; docs/writing-a-node-type.md#the-class): private
// to the type, shared by airplay.cpp and its tests, which run it through ear::testing::TypeHarness<spec, Airplay> by the
// same bank handles. In the type's own named namespace, never an unnamed one (type_spec.hpp says why).
//
// The node composes three things the framework owns and names nothing they share: a pipe receiver on from_airplay (the
// join), a port the framework picks, and shairport-sync as the configuration's program, its config file naming the
// receiver's FIFO and its report feed. Its audio passes straight through to `out`; its reports reach the report loop,
// which announces its listing (reports.hpp).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ear/core/lifecycle.hpp"
#include "ear/core/program.hpp"

namespace ear::nodes::airplay {

/// What the node runs: built from source in the images (docker/Dockerfile's shairport-sync stage).
inline constexpr ProgramPath shairportSync{"/usr/local/bin/shairport-sync"};
/// AirPlay 1's audio as shairport-sync writes it into its pipe: pinned here and in its config (configText), never left
/// to shairport-sync's defaults, which are S32_LE at 48 kHz in a build with AirPlay 2.
inline constexpr PipeFormat airplay1Format = PipeFormat::s16le;
inline constexpr SampleRate airplay1Rate = SampleRate::fixed<44'100>();

/// How a sender's list shows the speaker: 1 to 50 bytes of valid UTF-8 with no control character, no '%' (shairport-sync
/// expands %h, %H, %v and %V in a name) and no '@' (the separator of a RAOP instance, `<device id>@<name>`).
class SpeakerName {
public:
	static std::optional<SpeakerName> parse(std::string_view text);
	const std::string& text() const noexcept { return text_; }
	friend bool operator==(const SpeakerName&, const SpeakerName&) = default;

private:
	explicit SpeakerName(std::string text) : text_(std::move(text)) {}
	std::string text_;
};

inline constexpr auto nameRow = setting::optional(
	"name", setting::text<SpeakerName::parse>("a speaker name: 1 to 50 bytes, no control characters, no % or @"));
inline constexpr auto out = bank::output<"out">;
inline constexpr auto fromAirplay = bank::insideInput<"from_airplay">;
class Reports;
inline constexpr auto reports = loop::strand<"reports", Reports>;
inline constexpr TypeSpec spec{
	.settings = SettingsTable<nameRow>{"airplay"}, .banks = Banks<out, fromAirplay>{}, .loops = Loops<reports>{}};
using Checked = CheckedOf<spec>;

/// The mDNS host label the node's listing names: "ear-" and the node's name, each '_' a '-', cut to 63 bytes (a DNS
/// label's most). A node's name is unique in ear, so two nodes never share a host.
std::string hostLabelOf(std::string_view node);

/// shairport-sync's config file, in libconfig's syntax: the "metadata" mDNS backend (its listing handed to the report
/// feed, which ear announces), the pipe output into `fifo` in airplay1Format at airplay1Rate, and the metadata feed.
/// A file and not flags: shairport-sync dies "pipename needed" when its config lacks pipe.name.
std::vector<ProgramPart> configText(part::Fifo fifo);

class Airplay {
public:
	explicit Airplay(NodeOf<spec>& node) : node_(node) {}
	static Members<spec> ports(const Checked&) { return {out(members(2)), fromAirplay(members(2))}; }
	Configured configure(CreateOf<spec>& cx, const Checked& s);

private:
	NodeOf<spec>& node_;
};

} // namespace ear::nodes::airplay
