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
/// AirPlay 2's audio as shairport-sync writes it into its pipe (the airplay2 setting): pinned in its config and in the
/// receiver's shape the same way.
inline constexpr PipeFormat airplay2Format = PipeFormat::s32le;
inline constexpr SampleRate airplay2Rate = SampleRate::fixed<48'000>();

/// An AirPlay 2 speaker's device id: 48 bits shaped as a MAC address, locally administered and unicast (its first
/// byte's low two bits 1 and 0). shairport-sync reads it in AirPlay 2 only; in AirPlay 1 its own (an MD5 of the speaker
/// name and the host's first MAC address) stands.
class DeviceId {
public:
	/// From this host's name and the node's: FNV-1a 64 of "<host>/<node>", its low 48 bits, the first byte made
	/// locally administered and unicast. Stable while both names are; unique on the network while host names are.
	static DeviceId of(std::string_view host, std::string_view node);
	std::uint64_t bits() const noexcept { return bits_; }
	/// libconfig's 64-bit integer, "0x", 12 uppercase hex digits and "L": shairport-sync's general.airplay_device_id.
	std::string configText() const;
	/// 12 uppercase hex digits: what comes before the "@" of the node's RAOP instance in AirPlay 2.
	std::string raopPrefix() const;
	friend bool operator==(const DeviceId&, const DeviceId&) = default;

private:
	explicit DeviceId(std::uint64_t bits) : bits_(bits) {}
	std::uint64_t bits_;
};

/// The node's nqptp channel (the shared memory nqptp keeps for it): "/ear-<node>" when that is at most 63 bytes, else
/// "/ear-<first 41 bytes of node>-<16 lowercase hex digits of FNV-1a 64 of node>", so every node name ear holds has
/// its own.
SharedMemoryName channelOf(std::string_view node);

/// This host's name (gethostname(2)), read at configure: what DeviceId::of takes beside the node's name.
std::string thisHostName();

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
