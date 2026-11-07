// The "card" node type through its type harness, by the banks its spec declares (card_spec.hpp), which records the
// sound cards it acquires (docs/design/batch2.md section 4.4, slice E2 criterion 6; docs/design/node-framework.md
// slices 1 and 2: named inside banks, the framework's pass-through, the type harness). The engine side of a card (AlsaSoundCards) is tested in
// src/pipewire/tests/sound_card_test.cpp.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "card_spec.hpp"
#include "catch2/catch.hpp"
#include "ear/nodes/card.hpp"
#include "ear/testing/type_harness.hpp"

namespace {

namespace card = ear::nodes::card;
using Names = std::vector<ear::PortName>;
using State = ear::testing::NodeHarness::State;
using Harness = ear::testing::TypeHarness<card::spec, card::Card>;

/// A harness on a node named "motu" whose settings parsed.
Harness parsed(const ear::NodeSettings& settings) {
	Harness harness(ear::AudioFormat{48000, 256}, nullptr, "motu");
	const auto parse = harness->parse(settings);
	INFO(settings.dump() << ": " << (parse ? "" : parse.error()));
	REQUIRE(parse);
	return harness;
}

std::vector<ear::SoundCardRequest> cardsOf(Harness& harness) {
	std::vector<ear::SoundCardRequest> cards;
	for (const auto& request : harness->requests()) cards.push_back(std::get<ear::SoundCardRequest>(request));
	return cards;
}

} // namespace

TEST_CASE("Card accepts a card id with playback and capture channel counts", "[card]") {
	// The type its library makes is the one the harness runs.
	CHECK(ear::nodes::makeCardNodeType()->name() == "card");
	CHECK(Harness().type().name() == "card");
	auto harness = parsed({{"card", "Pro24"}, {"playback", 3}, {"capture", 2}});
	const ear::Ports& ports = harness->ports();
	CHECK(ports.inputs() == Names{"play.1", "play.2", "play.3"});
	CHECK(ports.insideOutputs() == Names{"to_card.1", "to_card.2", "to_card.3"});
	CHECK(ports.outputs() == Names{"capture.1", "capture.2"});
	CHECK(ports.insideInputs() == Names{"from_card.1", "from_card.2"});
	CHECK(ports.bank(card::toCard.name).reach == ear::PortBank::Reach::inside);
	CHECK(ports.bank(card::fromCard.name).reach == ear::PortBank::Reach::inside);

	// Each direction defaults to 0 channels.
	auto playbackOnly = parsed({{"card", "Test"}, {"playback", 1}});
	CHECK(playbackOnly->ports().inputs() == Names{"play.1"});
	CHECK(playbackOnly->ports().outputs().empty());
	CHECK(playbackOnly->ports().insideInputs().empty());
	auto captureOnly = parsed({{"card", "Test"}, {"capture", 64}});
	CHECK(captureOnly->ports().inputs().empty());
	CHECK(captureOnly->ports().outputs().size() == 64);
	CHECK(captureOnly->ports().outputs().back() == "capture.64");
}

TEST_CASE("Card rejects settings outside section 4.4's rules", "[card]") {
	const std::vector<ear::NodeSettings> invalid{
		// A raw ALSA device name: the file plugin's "|command" form, and hw:0 under an unknown key.
		{{"card", "file:'|touch x'"}, {"playback", 2}},
		{{"device", "hw:0"}},
		{{"device", "hw:0"}, {"card", "Pro24"}, {"playback", 2}},
		{{"card", "hw:0"}, {"playback", 2}},
		{{"card", "0123456789abcdef"}, {"playback", 2}}, // 16 characters
		{{"card", ""}, {"playback", 2}},
		{{"card", 3}, {"playback", 2}},
		{{"playback", 2}},   // no card
		{{"card", "Pro24"}}, // no channels
		{{"card", "Pro24"}, {"playback", 0}, {"capture", 0}},
		{{"card", "Pro24"}, {"playback", 65}},
		{{"card", "Pro24"}, {"capture", -1}},
		{{"card", "Pro24"}, {"playback", 1.5}},
		{{"card", "Pro24"}, {"playback", "2"}},
		{{"card", "Pro24"}, {"rate", 48000}, {"playback", 2}},
		ear::NodeSettings{},
		ear::NodeSettings(3),
	};
	for (const auto& settings : invalid) {
		INFO(settings.dump());
		Harness harness;
		const auto parse = harness->parse(settings);
		REQUIRE_FALSE(parse);
		CHECK_FALSE(parse.error().empty());
	}
	// The rule across settings is the type's check, run at parse: still invalid settings, never a failed start.
	Harness harness;
	CHECK(harness->parse({{"card", "Pro24"}}).error() == "playback or capture: at least one must be above 0");
}

TEST_CASE("Card start acquires one sound card per direction, each joined to its inside bank", "[card]") {
	auto harness = parsed({{"card", "Pro24"}, {"playback", 2}, {"capture", 3}});
	REQUIRE(harness->start());
	const auto cards = cardsOf(harness);
	REQUIRE(cards.size() == 2);
	const ear::CardId pro24 = *ear::CardId::parse("Pro24");
	CHECK(cards[0].card == pro24);
	CHECK(cards[0].direction == ear::CardDirection::playback);
	CHECK(cards[0].bank == card::toCard.name);
	CHECK(cards[1].card == pro24);
	CHECK(cards[1].direction == ear::CardDirection::capture);
	CHECK(cards[1].bank == card::fromCard.name);
	CHECK(harness->outsideName(0) == "motu.playback");
	CHECK(harness->outsideName(1) == "motu.capture");

	// Running once both cards are; a card lost fails the node engine-refused.
	CHECK(harness->state() == State::starting);
	harness->ready(0);
	CHECK(harness->state() == State::starting);
	harness->ready(1);
	CHECK(harness->state() == State::running);
	CHECK(harness->runObject() == nullptr);
	harness->lose(1, "card lost");
	CHECK(harness->state() == State::failed);
	CHECK(harness->failure()->code == ear::FailureReason::Code::engineRefused);

	auto playback = parsed({{"card", "Test"}, {"playback", 1}});
	REQUIRE(playback->start());
	REQUIRE(cardsOf(playback).size() == 1);
	CHECK(cardsOf(playback)[0].direction == ear::CardDirection::playback);

	auto capture = parsed({{"card", "Test"}, {"capture", 1}});
	REQUIRE(capture->start());
	REQUIRE(cardsOf(capture).size() == 1);
	CHECK(cardsOf(capture)[0].direction == ear::CardDirection::capture);
}

TEST_CASE("Card writes no audio code: the framework's pass-through carries play.i to to_card.i and from_card.j to "
          "capture.j sample for sample",
          "[card]") {
	constexpr std::uint32_t playback = 3;
	constexpr std::uint32_t capture = 2;
	constexpr std::uint32_t frames = 200;
	auto harness = parsed({{"card", "Pro24"}, {"playback", playback}, {"capture", capture}});
	REQUIRE(harness->start());
	harness->ready(0);
	harness->ready(1);

	// Each input carries its own distinct ramp.
	auto ramp = [](std::uint32_t channel, std::uint32_t frame) {
		return static_cast<float>(channel + 1) + static_cast<float>(frame) / 1000.0F;
	};
	const auto fill = [&](std::span<float> buffer, std::uint32_t channel) {
		for (std::uint32_t f = 0; f < buffer.size(); ++f) buffer[f] = ramp(channel, f);
	};
	const auto carried = [&](std::span<const float> buffer, std::uint32_t channel) {
		for (std::uint32_t f = 0; f < frames; ++f)
			if (buffer[f] != ramp(channel, f)) return false;
		return true;
	};
	for (std::uint32_t i = 0; i < playback; ++i) fill(harness.input(card::play, i), i);
	for (std::uint32_t j = 0; j < capture; ++j) fill(harness.input(card::fromCard, j), playback + j);
	harness->process(ear::GraphCycle{0, 0}, frames);
	CHECK(harness->unwritten() == 0);

	for (std::uint32_t i = 0; i < playback; ++i) {
		INFO("play." << i + 1 << " -> to_card." << i + 1);
		CHECK(carried(harness.output(card::toCard, i), i));
	}
	for (std::uint32_t j = 0; j < capture; ++j) {
		INFO("from_card." << j + 1 << " -> capture." << j + 1);
		CHECK(carried(harness.output(card::capture, j), playback + j));
	}
	harness->close();
}
