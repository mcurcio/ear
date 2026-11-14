// A type's stores through a real NodeHost, by the harness and its in-memory disk (docs/writing-a-node-type.md#stores):
// each store opened as a job on the I/O executor at readiness step `opens`, before any loop starts; a file there
// narrowed before its text is judged and never replaced when distrusted, a missing one created; the node configured only
// once every store is trusted; writes through the I/O executor, refused when the file changed; a completion that lands
// once its configuration ended dropped and its handle released on the I/O executor; an adapter's throw failing the node
// lost. The disk on real files and threads is ear_runtime's (stores_test.cpp).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace store_test {

using namespace ear;
using namespace std::chrono_literals;

/// What the type did, in order (the harness runs everything on the test's thread).
std::vector<std::string> events;
void said(std::string what) { events.push_back(std::move(what)); }
std::size_t count(const std::string& what) { return static_cast<std::size_t>(std::ranges::count(events, what)); }

/// What each test asks of the type.
struct Behaviour {
	bool readInConfigure = false;         // configure reads the store before it was opened
	std::vector<std::string> writes;      // start() posts a replace of each, in order
	std::vector<std::string> laterWrites; // and a replaceLater of each
};
Behaviour behaviour;

std::string newKey() { return "key 1"; }
/// The store's mode when its text was judged, read from the harness's disk.
std::optional<std::uint32_t> modeWhenJudged;
std::function<std::uint32_t()> modeNow;
std::optional<std::string> whyNotKey(std::string_view text) {
	if (modeNow) modeWhenJudged = modeNow();
	said("judged");
	if (!text.starts_with("key ")) return "malformed: not a key";
	return std::nullopt;
}

class Worker;
constexpr SettingsTable<> table{"kept"};
constexpr auto out = bank::output<"out">;
constexpr auto key = store::file<"key">({.mode = 0600, .maxBytes = 64, .initial = &newKey, .distrust = &whyNotKey});
constexpr auto worker = loop::dedicated<"worker", Worker>;
constexpr TypeSpec spec{.settings = table, .banks = Banks<out>{}, .stores = Stores<key>{}, .loops = Loops<worker>{}};
using Checked = CheckedOf<spec>;

/// The worker loop's state: it keeps the store, reads it as it is made on its loop's first turn, and writes it when told.
class Worker {
public:
	Worker(NodeHandle& handle, Store& store, Loop& self) : handle_(handle), store_(store), self_(self) {}
	void begin() { handle_.say("read " + store_.text()); }
	void write(std::string text) {
		auto written = store_.replace(std::move(text));
		handle_.say(written ? "written" : "write failed: " + written.error());
	}
	void writeLater(std::string text) {
		store_.replaceLater(std::move(text), self_, [this, text](std::expected<void, std::string> written) {
			handle_.say(written ? "written later " + text : "later write failed: " + written.error());
		});
	}

private:
	NodeHandle& handle_;
	Store& store_;
	Loop& self_;
};

struct Silence {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (auto member : io[out]) std::ranges::fill(member, 0.0F);
	}
};

class Kept {
public:
	explicit Kept(NodeOf<spec>& node) : node_(node) {}
	static Members<spec> ports(const Checked&) { return {out(members(1))}; }
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		Store& store = cx.store(key);
		if (behaviour.readInConfigure) said(store.text());
		cx.loop(worker, std::make_unique<Worker>(node_.handle(), store, cx.executor(worker)));
		cx.post(worker, [](Worker& w) { w.begin(); }); // staged: runs once configured, so the store is trusted
		cx.use(Silence{});
		return Configured::applied();
	}
	void start() {
		for (const std::string& text : behaviour.writes) node_.post(worker, [text](Worker& w) { w.write(text); });
		for (const std::string& text : behaviour.laterWrites)
			node_.post(worker, [text](Worker& w) { w.writeLater(text); });
	}

private:
	NodeOf<spec>& node_;
};

using Harness = testing::TypeHarness<spec, Kept>;

std::unique_ptr<Harness> fresh(Harness::Options options = {}, Behaviour asked = {}) {
	events.clear();
	behaviour = asked;
	modeWhenJudged.reset();
	modeNow = nullptr;
	options.node = "porch";
	auto harness = std::make_unique<Harness>(std::move(options));
	REQUIRE_FALSE(harness->parse({}));
	return harness;
}

std::string failure(const Harness& h) {
	const NodeHealth health = h.health();
	return health.reason ? std::string(failureCode(health.reason->code)) + ": " + health.reason->detail : "";
}

bool said(Harness& h, std::string_view note) { return std::ranges::find(h->notes(), note) != h->notes().end(); }

} // namespace store_test

using namespace store_test;

TEST_CASE(
	"A store is opened as one I/O job at readiness step opens, before any loop starts, and the node is configured "
	"once it is trusted, and only then does loop work read it",
	"[stores]") {
	auto h = fresh({.holdIo = true});
	h->add();
	CHECK(h->phase() == "configuring");
	CHECK(h->node().loops().empty()); // loops are a later step
	CHECK(h->node().ioJobs().empty());
	h->node().advance(2s);
	CHECK(h->health().waitingFor == std::vector<WaitingFor>{{WaitingFor::Kind::store, "kept-porch.key"},
	                                                        {WaitingFor::Kind::loop, "worker"}}); // its step is later

	h->runIo();
	CHECK(h->node().ioJobs() == std::vector<std::string>{"open store kept-porch.key"});
	REQUIRE(h->node().loops().size() == 1);
	CHECK(h->phase() == "starting"); // configured, waiting for its block
	h->runLoops();
	CHECK(said(*h, "read key 1"));
}

TEST_CASE("A missing store is created with its rule's initial text and exactly its mode", "[stores]") {
	auto h = fresh();
	h->add();
	CHECK(h->storeText(key) == "key 1");
	CHECK(h->storeMode(key) == 0600);
	CHECK(h->node().storeCalls() ==
	      std::vector<std::string>{"read kept-porch.key", "replace kept-porch.key"}); // nothing there to narrow
}

TEST_CASE("A store there is read and narrowed to its mode, every open", "[stores]") {
	auto h = fresh();
	h->presetStore(key, "key 7", 0664);
	h->add();
	h->blockReady();
	h->runLoops();
	CHECK(said(*h, "read key 7"));
	CHECK(h->storeMode(key) == 0600);
	CHECK(h->storeText(key) == "key 7");
}

TEST_CASE("A refused file is narrowed before it is read", "[stores]") {
	// The type's distrust reads the file's text only once its mode is narrowed: a file refused for its text may still
	// hold a secret, so it is never left wide (docs/writing-a-node-type.md#stores).
	auto h = fresh();
	modeNow = [&h] { return h->storeMode(key); };
	h->presetStore(key, "not a key", 0666);
	h->add();
	CHECK(h->phase() == "failed");
	CHECK(failure(*h) == "start-failed: kept-porch.key: malformed: not a key");
	REQUIRE(modeWhenJudged);
	CHECK(*modeWhenJudged == 0600);
	CHECK(h->storeMode(key) == 0600);
	CHECK(h->storeText(key) == "not a key"); // never a new identity over distrusted text
	CHECK(h->node().loops().empty());        // nothing of the node ran
	CHECK(count("judged") == 1);
}

TEST_CASE("A store that cannot be read fails the node start-failed naming the file, and no loop starts", "[stores]") {
	auto h = fresh();
	h->failStoreOpen(key, "Permission denied");
	h->add();
	CHECK(failure(*h) == "start-failed: kept-porch.key: Permission denied");
	CHECK(h->node().loops().empty());
	CHECK_FALSE(h->storeText(key)); // never created over a file it could not read
}

TEST_CASE("A store's open past its 5 s bound fails the node, its late completion dropped and its handle released on "
          "the I/O executor",
          "[stores]") {
	SECTION("the bound passes") {
		auto h = fresh({.holdIo = true});
		h->add();
		h->node().advance(5s);
		CHECK(failure(*h) == "start-failed: kept-porch.key: not opened within 5 s");
		h->runIo(); // the open runs late; its configuration has ended
		CHECK(h->node().ioJobs() ==
		      std::vector<std::string>{"open store kept-porch.key", "release store kept-porch.key"});
		CHECK(failure(*h) == "start-failed: kept-porch.key: not opened within 5 s");
		CHECK(h->node().loops().empty());
	}
	SECTION("the node is removed first") {
		auto h = fresh({.holdIo = true});
		h->add();
		h->remove();
		CHECK(h->gone());
		h->runIo();
		CHECK(h->node().ioJobs() ==
		      std::vector<std::string>{"open store kept-porch.key", "release store kept-porch.key"});
		CHECK(h->node().loops().empty());
	}
}

TEST_CASE("A store adapter's throw in an open fails the node lost, told as an internal error", "[stores]") {
	auto h = fresh();
	h->node().breakStores("the disk is on fire");
	h->add();
	CHECK(failure(*h) == "lost: internal error in StoreAdapter::read: the disk is on fire");
	REQUIRE(h->node().internalErrors().size() == 1);
	const auto& told = h->node().internalErrors().front();
	CHECK(told.callback == "StoreAdapter::read");
	CHECK(told.node == "porch");
	CHECK(told.where == "the I/O job open store kept-porch.key");
	CHECK(told.what == "the disk is on fire");
	CHECK(h->node().loops().empty());
}

TEST_CASE("A store read before it was opened is the type's bug", "[stores]") {
	auto h = fresh({}, {.readInConfigure = true});
	h->add();
	CHECK(failure(*h) == "start-failed: configure threw: store kept-porch.key was read before it was opened");
}

TEST_CASE("A stop and a start keep the store open, a restart opens it again", "[stores]") {
	auto h = fresh();
	h->add();
	h->blockReady();
	REQUIRE(h->stop());
	REQUIRE(h->start());
	CHECK(std::ranges::count(h->node().ioJobs(), std::string("open store kept-porch.key")) == 1);
	REQUIRE(h->restart());
	CHECK(std::ranges::count(h->node().ioJobs(), std::string("open store kept-porch.key")) == 2);
}

TEST_CASE("A blocking replace writes through the I/O executor, swapping the text in only if the file is unchanged",
          "[stores]") {
	SECTION("written") {
		auto h = fresh({}, {.writes = {"key 2"}});
		h->add();
		h->blockReady();
		h->runLoops();
		CHECK(said(*h, "written"));
		CHECK(h->storeText(key) == "key 2");
		CHECK(std::ranges::count(h->node().ioJobs(), std::string("write store kept-porch.key")) == 1);
	}
	SECTION("changed by something else: theirs stands") {
		auto h = fresh({}, {.writes = {"key 2"}});
		h->add();
		h->presetStore(key, "key theirs");
		h->blockReady();
		h->runLoops();
		CHECK(said(*h, "write failed: kept-porch.key: changed by something else"));
		CHECK(h->storeText(key) == "key theirs");
	}
	SECTION("larger than the rule's maxBytes") {
		auto h = fresh({}, {.writes = {std::string(65, 'k')}});
		h->add();
		h->blockReady();
		h->runLoops();
		CHECK(said(*h, "write failed: kept-porch.key: larger than 64 bytes"));
		CHECK(h->storeText(key) == "key 1");
	}
	SECTION("refused by the disk") {
		auto h = fresh({}, {.writes = {"key 2"}});
		h->add();
		h->failNextWrite(key, "No space left on device");
		h->blockReady();
		h->runLoops();
		CHECK(said(*h, "write failed: kept-porch.key: No space left on device"));
		CHECK(h->storeText(key) == "key 1");
	}
	SECTION("a mode widened since the open is narrowed before the write") {
		auto h = fresh({}, {.writes = {"key 2"}});
		h->add();
		h->presetStore(key, "key 1", 0666);
		h->blockReady();
		h->runLoops();
		CHECK(said(*h, "written"));
		CHECK(h->storeMode(key) == 0600);
	}
}

TEST_CASE("replaceLater: two writes in flight both commit, in order, and each one's done runs on the loop",
          "[stores]") {
	auto h = fresh({.holdIo = true}, {.laterWrites = {"key 2", "key 3"}});
	h->add();
	h->runIo(); // the open
	h->blockReady();
	h->runLoops(); // both writes queued on the I/O executor, neither run
	CHECK(h->storeText(key) == "key 1");
	h->runIo();
	CHECK(h->storeText(key) == "key 3");
	h->runLoops();
	const auto& notes = h->node().notes();
	const auto second = std::ranges::find(notes, "written later key 2");
	const auto third = std::ranges::find(notes, "written later key 3");
	REQUIRE(second != notes.end());
	REQUIRE(third != notes.end());
	CHECK(second < third);
}

TEST_CASE("A store adapter's throw in a write fails the node lost, told as an internal error", "[stores]") {
	auto h = fresh({}, {.writes = {"key 2"}});
	h->add();
	h->node().breakStores("the disk is on fire");
	h->blockReady();
	h->runLoops();
	CHECK(failure(*h) == "lost: internal error in StoreAdapter::narrow: the disk is on fire");
	REQUIRE(h->node().internalErrors().size() == 1);
	CHECK(h->node().internalErrors().front().where == "the I/O job write store kept-porch.key");
}
