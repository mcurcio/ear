// The sendspin type through the harness (docs/writing-a-node-type.md#testing-a-type), with no network: its key store
// opened before its loops run, its loops armed once configured and never starting the library until the worker's tick
// runs (which these tests never let it: the clock only moves at advance()), its code passing `to` to the sink and the
// feed to `from`, the feed's latency on its path, and what a stop keeps and a restart, a disable and a removal end
// (docs/node-types.md#stop-restart-disable). The library's client itself is clock_test.cpp's and the system tests'.
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/testing/type_harness.hpp"
#include "sendspin_spec.hpp"

namespace {

using namespace ear::nodes::sendspin;
using Harness = ear::testing::TypeHarness<spec, Sendspin>;

constexpr ear::AudioFormat format{.rate = ear::SampleRate::fixed<48000>(), .maxBlock = 256};

/// A node "ma" added with `settings`, its block not yet ready.
std::unique_ptr<Harness> added(const ear::NodeSettings& settings = ear::NodeSettings::object()) {
	auto h = std::make_unique<Harness>(Harness::Options{.format = format, .node = "ma"});
	REQUIRE_FALSE(h->parse(settings));
	h->add();
	return h;
}

/// Added, its block ready and running, what was staged run on its loops.
std::unique_ptr<Harness> running(const ear::NodeSettings& settings = ear::NodeSettings::object()) {
	auto h = added(settings);
	h->blockReady();
	REQUIRE(h->phase() == "running");
	h->runLoops();
	return h;
}

std::string failure(const Harness& h) {
	const ear::NodeHealth health = h.health();
	return health.reason ? std::string(ear::failureCode(health.reason->code)) + ": " + health.reason->detail : "";
}

} // namespace

TEST_CASE("A missing key file is created as an empty key record at 0600 before the loops run, which arm and start "
          "nothing yet",
          "[sendspin][type]") {
	const ear::NodeSettings settings =
		GENERATE(ear::NodeSettings::object(), ear::NodeSettings{{"server", "ws://10.0.0.1:8927/sendspin"}});
	INFO(settings.dump());
	auto h = running(settings);
	const auto text = h->storeText(keyFile);
	REQUIRE(text.has_value());
	CHECK_FALSE(whyNotKeyRecord(*text).has_value());
	CHECK(h->storeMode(keyFile) == 0600);
	CHECK(h->storeFile(keyFile) == "sendspin-ma.key");
	CHECK(h->armed(worker) == 1);   // the tick, whose first run would make the client
	CHECK(h->armed(notifier) == 1); // the watch on the code's bell
	CHECK(h->node().notes().empty());
	h->remove();
	CHECK(h->node().diagnostics().empty()); // every source the states armed went with them
}

TEST_CASE("A key file that is no key record fails the node start-failed before it ever ran, unchanged but for its "
          "mode",
          "[sendspin][type]") {
	auto h = std::make_unique<Harness>(Harness::Options{.format = format, .node = "ma"});
	REQUIRE_FALSE(h->parse(ear::NodeSettings::object()));
	h->presetStore(keyFile, "{ this is no key file", 0640);
	h->add();
	CHECK(h->phase() == "failed");
	CHECK(failure(*h) == "start-failed: sendspin-ma.key: malformed: not JSON");
	CHECK(h->storeText(keyFile) == "{ this is no key file");
	CHECK(h->storeMode(keyFile) == 0600); // a refused file may still hold a private key
	CHECK(h->node().loops().empty());     // no loop was made: the store's step comes first
}

TEST_CASE("The inputs reach the sink's bank side for side whatever the feed holds, and the outputs play the empty "
          "feed's zeros",
          "[sendspin][type]") {
	auto h = running();
	std::ranges::fill(h->input(to, 0), 0.25F);
	std::ranges::fill(h->input(to, 1), -0.5F);
	h->process(format.maxBlock);
	CHECK(h->unwritten() == 0);
	CHECK(h->output(toServer, 0)[0] == 0.25F);
	CHECK(h->output(toServer, 1)[format.maxBlock - 1] == -0.5F);
	CHECK(std::ranges::all_of(h->output(from, 0), [](float sample) { return sample == 0.0F; }));
	const auto& chunks = h->companion(toServer).chunks;
	REQUIRE(chunks.size() == 1); // handed to ToServer, which drops it while the server asks for nothing
	CHECK(chunks[0].frames == format.maxBlock);
	CHECK(chunks[0].interleaved[0] == 0.25F);
	CHECK(chunks[0].interleaved[1] == -0.5F);
}

TEST_CASE("The feed is the latency on the path from what the server sends to the outputs, and `to` reaches the sink's "
          "bank with none",
          "[sendspin][type]") {
	auto h = running();
	const ear::LatencyPlan& plan = h->node().latencyPlan();
	const auto onPair = [&](const char* from, const char* to) -> std::optional<ear::Latency> {
		for (const auto& [pair, delay] : plan.onPair)
			if (pair == ear::BankPair{from, to}) return delay.latency;
		return std::nullopt;
	};
	CHECK(onPair("from_server", "from") == ear::Latency{0, feedFramesFor(format)});
	CHECK(onPair("to", "to_server") == ear::Latency{});
	CHECK(feedFramesFor(format) == 2048); // 40 ms at 48 kHz, to the next power of two
}

TEST_CASE("A stop and a start keep the configuration whole, the worker's tick armed through the stop, and a restart, a "
          "disable and a removal end it",
          "[sendspin][type]") {
	auto h = running();
	REQUIRE(h->stop());
	h->runLoops();
	CHECK(h->phase() == "stopped");
	// What keeps the connection (docs/node-types.md#stop-restart-disable): the worker's state, its client and its tick.
	CHECK(h->armed(worker) == 1);
	CHECK(h->armed(notifier) == 1);
	REQUIRE(h->node().loops().size() == 2);
	CHECK_FALSE(h->node().loops()[0].finished);
	REQUIRE(h->start());
	h->runLoops();
	CHECK(h->phase() == "running");
	CHECK(h->node().loops().size() == 2);
	CHECK(h->armed(worker) == 1);

	REQUIRE(h->restart());
	h->runLoops();
	CHECK(h->phase() == "running");
	REQUIRE(h->node().loops().size() == 4); // the old configuration's two, finished and closed, and the new one's
	CHECK(h->node().loops()[0].finished);
	CHECK(h->node().loops()[1].closed);
	CHECK(h->armed(worker) == 1);

	h->disable();
	CHECK(h->phase() == "disabled");
	CHECK(std::ranges::all_of(h->node().loops(), [](const auto& loop) { return loop.finished && loop.closed; }));
	h->enable();
	h->runLoops();
	CHECK(h->phase() == "running");
	h->remove();
	CHECK(h->gone());
	CHECK(h->node().diagnostics().empty());
}
