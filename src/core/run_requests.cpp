// The checks and names every acquire of a run shares (ear/core/run_requests.hpp).
#include "ear/core/run_requests.hpp"

#include <stdexcept>
#include <string>
#include <variant>

namespace ear {
namespace {

template <typename... Kinds> struct Overloaded : Kinds... {
	using Kinds::operator()...;
};

const Ports::Bank& insideBank(const Ports& ports, const NodeName& node, const std::string& name, Direction direction) {
	const Ports::Bank& bank = ports.bank(name);
	if (bank.reach != PortBank::Reach::inside || bank.direction != direction)
		throw std::logic_error("node " + node + ": an outside node joins an inside " +
		                       (direction == Direction::input ? "input" : "output") + " bank, not \"" + name + "\"");
	return bank;
}

} // namespace

std::optional<std::string_view> outsideSuffix(const ResourceRequest& request) noexcept {
	return std::visit(Overloaded{
						  [](const SoundCardRequest& card) -> std::optional<std::string_view> {
							  return card.direction == CardDirection::playback ? "playback" : "capture";
						  },
						  [](const RtpReceiverRequest&) -> std::optional<std::string_view> { return "rtp"; },
						  [](const auto&) -> std::optional<std::string_view> { return std::nullopt; },
					  },
	                  request);
}

const Ports::Bank* requestedBank(const Ports& ports, const NodeName& node, const ResourceRequest& request) {
	return std::visit(
		Overloaded{
			[&](const SoundCardRequest& card) -> const Ports::Bank* {
				return &insideBank(ports, node, card.bank,
		                           card.direction == CardDirection::playback ? Direction::output : Direction::input);
			},
			[&](const RtpReceiverRequest& receiver) -> const Ports::Bank* {
				return &insideBank(ports, node, receiver.bank, Direction::input);
			},
			[&](const HandoffRequest& handoff) -> const Ports::Bank* {
				const Ports::Bank& bank = ports.bank(handoff.bank);
				if (bank.direction != Direction::input || handoff.consumer == nullptr)
					throw std::logic_error("node " + node + ": a handoff takes an input bank and a consumer, not \"" +
			                               handoff.bank + "\"");
				return &bank;
			},
			[](const RecordingRequest&) -> const Ports::Bank* { return nullptr; },
		},
		request);
}

} // namespace ear
