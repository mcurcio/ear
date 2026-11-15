// The "airplay" node type: an AirPlay 1 speaker, one shairport-sync per node (docs/node-types.md#airplay,
// ear/nodes/airplay.hpp; docs/writing-a-node-type.md#programs). Its spec is airplay_spec.hpp's, its reports reports.hpp's.
#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "airplay_spec.hpp"
#include "ear/core/latency.hpp"
#include "ear/mdns/advertiser.hpp"
#include "ear/nodes/airplay.hpp"
#include "reports.hpp"

namespace ear::nodes {
namespace airplay {
namespace {

/// The format's name in shairport-sync's config: the one place airplay1Format becomes text.
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
	// UTF-8 with no control character: the one home of that rule is the advertiser's, which announces the name in the
	// instance shairport-sync makes of it.
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

std::vector<ProgramPart> configText(part::Fifo fifo) {
	return {part::Text{"general = { mdns_backend = \"metadata\"; output_backend = \"pipe\"; };\n"
	                   "metadata = { enabled = \"yes\"; pipe_name = \""},
	        part::Feed{}, part::Text{"\"; };\npipe = { name = \""}, std::move(fifo),
	        part::Text{"\"; output_format = \"" + std::string(formatName(airplay1Format)) + "\"; output_rate = " +
	                   std::to_string(FrameClock(airplay1Rate).frames(std::chrono::seconds(1))) + "; };\n"}};
}

Configured Airplay::configure(CreateOf<spec>& cx, const Checked& s) {
	// The pipe module's fixed buffer is latency beyond from_airplay, which the framework adds
	// (docs/writing-a-node-type.md#engine-companions): nothing to declare.
	cx.pipeReceiver(fromAirplay, PipeReceiverShape{.format = airplay1Format, .rate = airplay1Rate});
	const GrantedPort rtsp = cx.claim(claim::AnyTcp{});
	cx.loop(reports, std::make_unique<Reports>(node_.handle(), cx.executor(reports), hostLabelOf(cx.node())));
	const std::optional<SpeakerName> name = s.get<nameRow>();
	cx.program(
		ProgramShape{
			.executable = shairportSync,
			.args =
				{"-c", {part::File{"config"}}, "--name", name ? name->text() : cx.node(), "--port", {part::Port{rtsp}}},
			.files = {{"config", configText(cx.fifoOf(fromAirplay))}}},
		reports, &Reports::onFeed);
	cx.use(passThrough({bankPair(fromAirplay, out)}));
	return Configured::applied();
}

} // namespace airplay

std::unique_ptr<const NodeType> makeAirplayNodeType() { return makeNodeType<airplay::spec, airplay::Airplay>(); }

} // namespace ear::nodes
