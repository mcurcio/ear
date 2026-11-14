// The node handle (docs/writing-a-node-type.md#the-node-handle) through a real NodeHost, by the harness: notes paced
// per configuration, a loss never held back, a log entry carrying its node, and nothing of a configuration that ended
// landing anywhere. The rate on its own clock is note_gate_test.cpp's.
#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace node_handle_test {

using namespace ear;
using namespace std::chrono_literals;

constexpr SettingsTable<> table{"speaker"};
constexpr auto in = bank::input<"in">;
constexpr auto out = bank::output<"out">;
constexpr TypeSpec spec{.settings = table, .banks = Banks<in, out>{}};
using Checked = CheckedOf<spec>;

/// The handle of the newest configuration, as a type's own thread would keep it; null before the first.
NodeHandle* handle = nullptr;
/// How many configurations were made.
int configured = 0;
/// What the type says through its handle as its class is destroyed: after its configuration ended, so dropped.
bool sayAsDestroyed = false;

class Speaker {
public:
	explicit Speaker(NodeOf<spec>& node) : node_(node) {}
	~Speaker() {
		if (!sayAsDestroyed) return;
		node_.handle().say("from an ended configuration");
		node_.handle().warn("logged by an ended configuration");
		node_.handle().lost("lost by an ended configuration");
	}
	Speaker(const Speaker&) = delete;
	Speaker& operator=(const Speaker&) = delete;
	static Members<spec> ports(const Checked&) { return {in(members(1)), out(members(1))}; }
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		handle = &node_.handle();
		++configured;
		cx.use(passThrough({bankPair(in, out)}));
		return Configured::applied();
	}
	void stop() { node_.handle().say("goodbye"); }

private:
	NodeOf<spec>& node_;
};

using Harness = testing::TypeHarness<spec, Speaker>;

/// A running node "hall", its handle kept.
std::unique_ptr<Harness> running() {
	handle = nullptr;
	configured = 0;
	sayAsDestroyed = false;
	auto harness = std::make_unique<Harness>(Harness::Options{.node = "hall"});
	REQUIRE_FALSE(harness->parse({}));
	harness->add();
	harness->blockReady();
	REQUIRE(harness->phase() == "running");
	REQUIRE(handle != nullptr);
	return harness;
}

/// What the main loop has queued runs (the harness's test thread stands in for it).
void pump(Harness& harness) { harness->advance(0ms); }

std::size_t count(const std::vector<std::string>& notes, const std::string& note) {
	return static_cast<std::size_t>(std::ranges::count(notes, note));
}

} // namespace node_handle_test
using namespace node_handle_test;

TEST_CASE("A handle names its node", "[node_handle]") {
	const auto owner = running();
	CHECK(handle->node() == "hall");
}

TEST_CASE("Notes pass 32 at once, then one a second, and the framework says how many were held back before the next",
          "[node_handle]") {
	auto owner = running();
	Harness& harness = *owner;
	for (int i = 0; i < 40; ++i) handle->say("note " + std::to_string(i));
	pump(harness);
	const auto& notes = harness->notes();
	REQUIRE(notes.size() == 32);
	CHECK(notes.front() == "note 0");
	CHECK(notes.back() == "note 31");

	handle->say("too soon");
	pump(harness);
	CHECK(notes.size() == 32);

	harness->advance(1s);
	handle->say("next");
	pump(harness);
	REQUIRE(notes.size() == 34);
	CHECK(notes[32] == "notes-dropped 9");
	CHECK(notes[33] == "next");
}

TEST_CASE("A stop and a start keep the configuration's rate", "[node_handle]") {
	auto owner = running();
	Harness& harness = *owner;
	NodeHandle* const kept = handle;
	for (int i = 0; i < 32; ++i) handle->say("note " + std::to_string(i));
	REQUIRE(harness->stop());
	REQUIRE(harness->phase() == "stopped");
	REQUIRE(harness->start());
	REQUIRE(harness->phase() == "running");
	REQUIRE(configured == 1); // the same configuration
	REQUIRE(handle == kept);
	handle->say("after the start");
	pump(harness);
	// The stop's goodbye was held back too: the burst was spent before it.
	CHECK(harness->notes().size() == 32);
	CHECK(count(harness->notes(), "after the start") == 0);
	CHECK(count(harness->notes(), "goodbye") == 0);
}

TEST_CASE("A restart begins a new configuration with a fresh rate", "[node_handle]") {
	auto owner = running();
	Harness& harness = *owner;
	for (int i = 0; i < 33; ++i) handle->say("note " + std::to_string(i));
	pump(harness);
	REQUIRE(harness->notes().size() == 32);
	REQUIRE(harness->restart());
	REQUIRE(harness->phase() == "running");
	REQUIRE(configured == 2);
	for (int i = 0; i < 31; ++i) handle->say("again " + std::to_string(i));
	pump(harness);
	const auto& notes = harness->notes();
	// The old configuration's goodbye was over its rate; the new one's burst is whole, and owes no count.
	REQUIRE(notes.size() == 32 + 31);
	CHECK(notes[32] == "again 0");
	CHECK(notes.back() == "again 30");
	CHECK(std::ranges::none_of(notes, [](const std::string& note) { return note.starts_with("notes-dropped"); }));
}

TEST_CASE("A loss is never held back by the note rate", "[node_handle]") {
	auto owner = running();
	Harness& harness = *owner;
	for (int i = 0; i < 100; ++i) handle->say("note");
	handle->lost("gone");
	pump(harness);
	const NodeHealth health = harness->health();
	CHECK(health.state == NodeState::failed);
	REQUIRE(health.reason);
	CHECK(health.reason->code == FailureReason::Code::lost);
	CHECK(health.reason->detail == "gone");
}

TEST_CASE("A goodbye said as the node stops is heard, its teardown begun", "[node_handle]") {
	auto owner = running();
	Harness& harness = *owner;
	harness->remove();
	CHECK(harness->notes() == std::vector<std::string>{"goodbye"});
	CHECK(harness->gone());
}

TEST_CASE("A log entry carries its node, and an empty source becomes the type's name while a given one is kept",
          "[node_handle]") {
	auto owner = running();
	Harness& harness = *owner;
	handle->info("plain");
	handle->log(LogEntry{.level = LogLevel::warn, .source = "library", .text = "its own", .topic = "net"});
	const std::vector<LogEntry> logs = harness->logs();
	REQUIRE(logs.size() == 2);
	CHECK(logs[0].level == LogLevel::info);
	CHECK(logs[0].source == "speaker");
	CHECK(logs[0].text == "plain");
	CHECK(logs[0].node == std::optional<NodeName>("hall"));
	CHECK(logs[1].level == LogLevel::warn);
	CHECK(logs[1].source == "library");
	CHECK(logs[1].topic == "net");
	CHECK(logs[1].node == std::optional<NodeName>("hall"));
	CHECK(harness->notes().empty()); // a log is no note
}

TEST_CASE("What an ended configuration says, logs or loses through its handle lands nowhere, the next one running",
          "[node_handle]") {
	auto owner = running();
	Harness& harness = *owner;
	sayAsDestroyed = true; // the old class speaks as it is freed: after its configuration ended, before the next began
	REQUIRE(harness->restart());
	sayAsDestroyed = false;
	REQUIRE(configured == 2);
	harness->advance(1s);
	CHECK(harness->phase() == "running");
	CHECK(count(harness->notes(), "from an ended configuration") == 0);
	CHECK(std::ranges::none_of(harness->logs(),
	                           [](const LogEntry& entry) { return entry.text == "logged by an ended configuration"; }));
	// The new configuration's handle is heard.
	handle->say("new");
	handle->info("new");
	pump(harness);
	CHECK(harness->notes().back() == "new");
	CHECK(harness->logs().back().text == "new");
}
