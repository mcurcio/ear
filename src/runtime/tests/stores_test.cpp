// The nodes' stores on real files and the real I/O thread (ear/runtime/stores.hpp; docs/writing-a-node-type.md#stores):
// ConfinedStores' disk part (read with no symlink and at most maxBytes, narrow, replace only if unchanged, a created
// file's exact mode), its wait for a job (ran, overdue, dropped by a stopped I/O thread), and the boot sweep over the
// declared stores only. Each wait is for the event it needs; a deadline only fails. What is read, judged and written
// when (the policy) is core's (store_test.cpp).
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/runtime/io_thread.hpp"
#include "ear/runtime/stores.hpp"
#include "temp_folder.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace stores_test {

using namespace ear;
using namespace std::chrono_literals;
using ear::runtime::ConfinedStores;
using ear::test::TempFolder;

/// For the tests whose jobs never throw: hearing a fault fails the test.
struct NoFaults final : InternalErrorListener {
	void internalError(const InternalError& error) override { FAIL("a fault: " << error.where << ": " << error.what); }
};

void write(const std::filesystem::path& file, const std::string& text, mode_t mode) {
	std::ofstream(file) << text;
	REQUIRE(::chmod(file.c_str(), mode) == 0);
}
mode_t modeOf(const std::filesystem::path& file) {
	struct stat seen{};
	REQUIRE(::stat(file.c_str(), &seen) == 0);
	return seen.st_mode & 07777;
}

/// What the file holds (nothing: missing), read through `stores` within 64 bytes; a failed read fails the test.
std::optional<std::string> readText(ConfinedStores& stores, const std::string& file) {
	auto read = stores.read(file, 64);
	REQUIRE(read);
	return *read;
}

std::shared_ptr<const folder::ConfinedFolder> opened(const TempFolder& dir) {
	auto folder = folder::ConfinedFolder::open(dir.path());
	REQUIRE(folder);
	return std::make_shared<const folder::ConfinedFolder>(std::move(*folder));
}

// Two declared types for the sweep: "kept" with a store "key" (0600) and "held" with "ssrc" (0640).
constexpr auto out = bank::output<"out">;
constexpr auto key = store::file<"key">({.mode = 0600, .maxBytes = 64});
constexpr TypeSpec keptSpec{.settings = SettingsTable<>{"kept"}, .banks = Banks<out>{}, .stores = Stores<key>{}};
constexpr auto ssrc = store::file<"ssrc">({.mode = 0640, .maxBytes = 16});
constexpr TypeSpec heldSpec{.settings = SettingsTable<>{"held"}, .banks = Banks<out>{}, .stores = Stores<ssrc>{}};
struct Silence {
	void process(Io<keptSpec>, const GraphCycle&) noexcept {}
};
struct Kept {
	static Members<keptSpec> ports(const CheckedOf<keptSpec>&) { return {out(members(1))}; }
	Configured configure(CreateOf<keptSpec>& cx, const CheckedOf<keptSpec>&) {
		cx.use(Silence{});
		return Configured::applied();
	}
};
struct HeldSilence {
	void process(Io<heldSpec>, const GraphCycle&) noexcept {}
};
struct Held {
	static Members<heldSpec> ports(const CheckedOf<heldSpec>&) { return {out(members(1))}; }
	Configured configure(CreateOf<heldSpec>& cx, const CheckedOf<heldSpec>&) {
		cx.use(HeldSilence{});
		return Configured::applied();
	}
};

} // namespace stores_test

using namespace stores_test;

TEST_CASE("ConfinedStores reads a regular file beneath its folder, at most maxBytes and through no symlink",
          "[stores]") {
	TempFolder dir("stores");
	NoFaults faults;
	runtime::IoThread io(faults);
	ConfinedStores stores(opened(dir), io);
	write(dir.path() / "kept-a.key", "secret", 0600);
	CHECK(readText(stores, "kept-a.key") == std::optional<std::string>("secret"));
	CHECK(readText(stores, "kept-b.key") == std::nullopt);
	CHECK_FALSE(stores.read("kept-a.key", 3));
	std::filesystem::create_symlink(dir.path() / "kept-a.key", dir.path() / "kept-c.key");
	const auto linked = stores.read("kept-c.key", 64);
	REQUIRE_FALSE(linked);
	CHECK(linked.error().starts_with("kept-c.key: "));
	io.stop();
}

TEST_CASE("ConfinedStores narrows a mode and never widens it", "[stores]") {
	TempFolder dir("stores");
	NoFaults faults;
	runtime::IoThread io(faults);
	ConfinedStores stores(opened(dir), io);
	write(dir.path() / "kept-a.key", "secret", 0664);
	REQUIRE(stores.narrow("kept-a.key", 0600));
	CHECK(modeOf(dir.path() / "kept-a.key") == 0600);
	REQUIRE(stores.narrow("kept-a.key", 0644));
	CHECK(modeOf(dir.path() / "kept-a.key") == 0600);
	CHECK_FALSE(stores.narrow("kept-b.key", 0600)); // missing
	io.stop();
}

TEST_CASE("ConfinedStores replaces a file only if it holds what was expected, and one it creates has exactly the mode",
          "[stores]") {
	TempFolder dir("stores");
	NoFaults faults;
	runtime::IoThread io(faults);
	ConfinedStores stores(opened(dir), io);
	const auto file = dir.path() / "kept-a.key";
	CHECK(stores.replaceIf("kept-a.key", std::nullopt, "one", 64, 0600) == StoreWrite::replaced);
	CHECK(modeOf(file) == 0600);
	CHECK(stores.replaceIf("kept-a.key", std::nullopt, "two", 64, 0600) == StoreWrite::changed);
	CHECK(stores.replaceIf("kept-a.key", std::string("one"), "two", 64, 0600) == StoreWrite::replaced);
	CHECK(readText(stores, "kept-a.key") == std::optional<std::string>("two"));
	CHECK(stores.replaceIf("kept-a.key", std::string("one"), "three", 64, 0600) == StoreWrite::changed);
	CHECK(readText(stores, "kept-a.key") == std::optional<std::string>("two"));
	io.stop();
}

TEST_CASE("ConfinedStores' wait: a job that ran, one past its bound, one a stopped I/O thread destroyed unrun",
          "[stores]") {
	TempFolder dir("stores");
	NoFaults faults;
	runtime::IoThread io(faults);
	ConfinedStores stores(opened(dir), io);
	SECTION("ran, on the I/O thread") {
		std::thread::id ranOn;
		CHECK(stores.await("a job", [&] { ranOn = std::this_thread::get_id(); }, 10s) == JobWait::ran);
		CHECK(ranOn != std::this_thread::get_id());
		io.stop();
	}
	SECTION("overdue: the job ahead of it holds the thread") {
		std::promise<void> release;
		std::shared_future<void> released = release.get_future().share();
		io.post("held", [released] { released.wait(); });
		bool ran = false;
		CHECK(stores.await("late", [&ran] { ran = true; }, 50ms) == JobWait::overdue);
		release.set_value();
		io.stop(); // the late job may run before the stop: overdue says only that the wait gave up
		static_cast<void>(ran);
	}
	SECTION("dropped: the I/O thread had stopped") {
		io.stop();
		bool ran = false;
		CHECK(stores.await("after the stop", [&ran] { ran = true; }, 10s) == JobWait::dropped);
		CHECK_FALSE(ran);
	}
}

TEST_CASE("The boot sweep narrows every declared store's files and their staging files, whatever their node, and "
          "nothing else",
          "[stores]") {
	TempFolder dir("sweep");
	write(dir.path() / "kept-gone.key", "a", 0666);
	write(dir.path() / ".kept-gone.key.ear-staging", "a", 0666);
	write(dir.path() / "held-x.ssrc", "b", 0666);
	write(dir.path() / "kept-x.ssrc", "c", 0666); // another type's suffix
	write(dir.path() / "ear.conf", "graph { }", 0666);
	std::vector<std::unique_ptr<const NodeType>> types;
	types.push_back(makeNodeType<keptSpec, Kept>());
	types.push_back(makeNodeType<heldSpec, Held>());
	const auto folder = opened(dir);
	CHECK(runtime::sweepStores(*folder, types).empty());
	CHECK(modeOf(dir.path() / "kept-gone.key") == 0600);
	CHECK(modeOf(dir.path() / ".kept-gone.key.ear-staging") == 0600);
	CHECK(modeOf(dir.path() / "held-x.ssrc") == 0640);
	CHECK(modeOf(dir.path() / "kept-x.ssrc") == 0666);
	CHECK(modeOf(dir.path() / "ear.conf") == 0666);
}
