// The "airplay" node type: an AirPlay 1 or AirPlay 2 speaker, one shairport-sync per node (docs/node-types.md#airplay,
// ear/nodes/airplay.hpp; docs/writing-a-node-type.md#programs). Its spec is airplay_spec.hpp's, its reports reports.hpp's.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <climits>
#include <unistd.h>

#include "airplay_spec.hpp"
#include "ear/core/latency.hpp"
#include "ear/mdns/advertiser.hpp"
#include "ear/nodes/airplay.hpp"
#include "reports.hpp"

namespace ear::nodes {
namespace airplay {
namespace {

/// FNV-1a, 64 bits: the one hash of AirPlay 2's identity (DeviceId::of, channelOf). Fixed by its definition, so a
/// node's identity stays the same across builds and restarts.
std::uint64_t fnv1a64(std::string_view text) {
	std::uint64_t hash = 0xcbf29ce484222325ULL;
	for (const char c : text) {
		hash ^= static_cast<unsigned char>(c);
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

/// The format's name in shairport-sync's config: the one place a PipeFormat becomes its text.
constexpr std::string_view formatName(PipeFormat format) {
	switch (format) {
	case PipeFormat::s16le:
		return "S16_LE";
	case PipeFormat::s32le:
		return "S32_LE";
	}
	return "S16_LE";
}

} // namespace

std::optional<SpeakerName> SpeakerName::parse(std::string_view text) {
	constexpr std::size_t most = 50;
	if (text.empty() || text.size() > most || text.find_first_of("%@") != std::string_view::npos) return std::nullopt;
	// UTF-8 with no control character and no dot: the one home of that rule is the advertiser's, which announces the
	// name in the instance shairport-sync makes of it.
	if (mdns::listingRefused(
			mdns::Listing{.service = "_raop._tcp", .instance = std::string(text), .host = "ear", .port = 1, .txt = {}}))
		return std::nullopt;
	return SpeakerName(std::string(text));
}

std::string hostLabelOf(std::string_view node) {
	constexpr std::size_t labelBytes = 63;
	std::string label = "ear-" + std::string(node);
	std::ranges::replace(label, '_', '-');
	if (label.size() > labelBytes) label.resize(labelBytes);
	return label;
}

DeviceId DeviceId::of(std::string_view host, std::string_view node) {
	constexpr std::uint64_t low48 = (std::uint64_t{1} << 48U) - 1;
	constexpr unsigned firstByte = 40;
	const std::uint64_t locallyAdministered = std::uint64_t{0x02} << firstByte;
	const std::uint64_t multicast = std::uint64_t{0x01} << firstByte;
	const std::uint64_t bits = fnv1a64(std::string(host) + "/" + std::string(node)) & low48;
	return DeviceId((bits | locallyAdministered) & ~multicast);
}

std::string DeviceId::configText() const { return std::format("0x{:012X}L", bits_); }

std::string DeviceId::raopPrefix() const { return std::format("{:012X}", bits_); }

SharedMemoryName channelOf(std::string_view node) {
	constexpr std::size_t most = 63; // SharedMemoryName's
	constexpr std::size_t kept = 41; // "/ear-", 41 bytes, "-", 16 hex digits: 63
	const std::string whole = "/ear-" + std::string(node);
	const std::string text =
		whole.size() <= most ? whole : std::format("/ear-{}-{:016x}", node.substr(0, kept), fnv1a64(node));
	auto name = SharedMemoryName::parse(text);
	// A node's name is [A-Za-z0-9_-] (docs/control-port.md#error-codes), all of which a shared-memory name holds.
	if (!name) throw std::logic_error("channelOf: node " + std::string(node) + " makes no shared-memory name");
	return *std::move(name);
}

std::string thisHostName() {
	std::array<char, HOST_NAME_MAX + 1> name{};
	if (::gethostname(name.data(), name.size() - 1) != 0) return {};
	return std::string(name.data());
}

PipeReceiverShape pipeShapeOf(const Protocol& protocol) {
	if (std::holds_alternative<Airplay2>(protocol)) return {.format = airplay2Format, .rate = airplay2Rate};
	return {.format = airplay1Format, .rate = airplay1Rate};
}

std::vector<ProgramPart> configText(part::Fifo fifo, const Protocol& protocol) {
	std::string general = "general = { mdns_backend = \"metadata\"; output_backend = \"pipe\"; service_type = ";
	if (const auto* two = std::get_if<Airplay2>(&protocol))
		general += "\"airplay2\"; ignore_volume_control = \"yes\"; udp_port_base = 0; airplay_device_id = " +
		           two->deviceId.configText() + "; nqptp_shared_memory_interface_name = \"" + two->channel.text() +
		           "\"; };\n";
	else general += "\"classic\"; ignore_volume_control = \"yes\"; udp_port_base = 0; };\n";
	const PipeReceiverShape shape = pipeShapeOf(protocol);
	return {part::Text{general + "metadata = { enabled = \"yes\"; pipe_name = \""}, part::Feed{},
	        part::Text{"\"; };\npipe = { name = \""}, std::move(fifo),
	        part::Text{"\"; output_format = \"" + std::string(formatName(shape.format)) + "\"; output_rate = " +
	                   std::to_string(FrameClock(shape.rate).frames(std::chrono::seconds(1))) + "; };\n"}};
}

Configured Airplay::configure(CreateOf<spec>& cx, const Checked& s) {
	const Protocol protocol = s.get<airplay2Row>().value_or(false)
	                              ? Protocol{Airplay2{DeviceId::of(thisHostName(), cx.node()), channelOf(cx.node())}}
	                              : Protocol{Airplay1{}};
	// The pipe module's fixed buffer is latency beyond from_airplay, which the framework adds
	// (docs/writing-a-node-type.md#engine-companions): nothing to declare.
	cx.pipeReceiver(fromAirplay, pipeShapeOf(protocol));
	const GrantedPort rtsp = cx.claim(claim::AnyTcp{});
	cx.loop(reports, std::make_unique<Reports>(node_.handle(), cx.executor(reports), hostLabelOf(cx.node())));
	const std::optional<SpeakerName> name = s.get<nameRow>();
	cx.program(
		ProgramShape{
			.executable = shairportSync,
			.args =
				{"-c", {part::File{"config"}}, "--name", name ? name->text() : cx.node(), "--port", {part::Port{rtsp}}},
			.files = {{"config", configText(cx.fifoOf(fromAirplay), protocol)}},
			// shairport-sync never notices nqptp leaving, so ear ends it when its channel goes
			// (docs/writing-a-node-type.md#programs).
			.needs = std::holds_alternative<Airplay2>(protocol)
	                     ? std::vector<SharedMemoryName>{std::get<Airplay2>(protocol).channel}
	                     : std::vector<SharedMemoryName>{}},
		reports, &Reports::onFeed);
	cx.use(passThrough({bankPair(fromAirplay, out)}));
	return Configured::applied();
}

} // namespace airplay

std::unique_ptr<const NodeType> makeAirplayNodeType() { return makeNodeType<airplay::spec, airplay::Airplay>(); }

} // namespace ear::nodes
