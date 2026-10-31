// The confined folder (docs/design/knobly-config.md) on real directories: opened at boot (made if
// missing, never its parents), create() as the recordings folder had it, read() and replaceIf()
// refusing every symlink, the held path, and the boot check that folders stay apart.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/folder/confined_folder.hpp"

namespace {

using ear::folder::Changed;
using ear::folder::ConfinedFolder;
using ear::folder::Replaced;

constexpr std::size_t maxBytes = 1024;

class TempDir {
public:
	explicit TempDir(const std::string& name)
	    : path_(std::filesystem::temp_directory_path() / ("ear_folder_" + name + "_" + std::to_string(getpid()))) {
		std::filesystem::remove_all(path_);
		std::filesystem::create_directories(path_);
	}
	~TempDir() {
		std::error_code ignored;
		std::filesystem::permissions(path_ / "folder", std::filesystem::perms::owner_all,
		                             std::filesystem::perm_options::add, ignored);
		std::filesystem::remove_all(path_, ignored);
	}
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;
	const std::filesystem::path& path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
};

ConfinedFolder openFolder(const std::filesystem::path& path) {
	auto folder = ConfinedFolder::open(path);
	REQUIRE(folder.has_value());
	return std::move(*folder);
}

std::string contents(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

/// What read() found, which must not be an error.
std::optional<std::string> readText(const ConfinedFolder& folder, const std::string& relative) {
	auto read = folder.read(relative, maxBytes);
	INFO(relative << ": " << (read ? std::string("read") : read.error().detail));
	REQUIRE(read.has_value());
	return std::move(*read);
}

std::filesystem::perms permissionsOf(const std::filesystem::path& path) {
	return std::filesystem::status(path).permissions() & std::filesystem::perms::mask;
}

/// Every name in `directory`, so a test can see that no staging file is left.
std::vector<std::string> names(const std::filesystem::path& directory) {
	std::vector<std::string> found;
	for (const auto& entry : std::filesystem::directory_iterator(directory)) {
		found.push_back(entry.path().filename().string());
	}
	std::ranges::sort(found);
	return found;
}

}  // namespace

// --- open() and create(), the recordings folder's contract, unchanged ---

TEST_CASE("A folder is made if missing and opened if it is there", "[folder]") {
	TempDir dir{"open"};
	const auto path = dir.path() / "recordings";

	const auto made = ConfinedFolder::open(path);
	REQUIRE(made.has_value());
	CHECK(std::filesystem::is_directory(path));
	CHECK(made->path() == path);

	const auto again = ConfinedFolder::open(path);
	CHECK(again.has_value());
}

TEST_CASE("A folder is never made with its parents, and a file is not a folder", "[folder]") {
	TempDir dir{"parents"};
	const auto deep = dir.path() / "missing" / "recordings";
	const auto missingParent = ConfinedFolder::open(deep);
	REQUIRE_FALSE(missingParent.has_value());
	CHECK(missingParent.error().detail == "folder " + deep.string() + ": cannot create: No such file or directory");
	CHECK_FALSE(std::filesystem::exists(dir.path() / "missing"));

	const auto file = dir.path() / "plain";
	write(file, "x");
	const auto notFolder = ConfinedFolder::open(file);
	REQUIRE_FALSE(notFolder.has_value());
	CHECK(notFolder.error().detail.find(file.string()) != std::string::npos);
}

TEST_CASE("create() makes or truncates a file beneath, following an inner symlink, never one out", "[folder]") {
	TempDir dir{"create"};
	const auto folder = openFolder(dir.path() / "folder");
	write(dir.path() / "outside.wav", "keep");
	write(dir.path() / "folder" / "old.wav", "old text");
	std::filesystem::create_directory(dir.path() / "folder" / "takes");
	std::filesystem::create_directory_symlink("takes", dir.path() / "folder" / "inner");
	std::filesystem::create_symlink(dir.path() / "outside.wav", dir.path() / "folder" / "out.wav");

	for (const std::string& relative : std::vector<std::string>{"new.wav", "old.wav", "inner/take.wav"}) {
		INFO(relative);
		const auto fd = folder.create(relative);
		REQUIRE(fd.has_value());
		CHECK(::write(*fd, "x", 1) == 1);
		::close(*fd);
	}
	CHECK(contents(dir.path() / "folder" / "new.wav") == "x");
	CHECK(contents(dir.path() / "folder" / "old.wav") == "x");
	CHECK(contents(dir.path() / "folder" / "takes" / "take.wav") == "x");

	for (const std::string& relative : std::vector<std::string>{"out.wav", "../outside.wav", (dir.path() / "outside.wav").string()}) {
		INFO(relative);
		const auto fd = folder.create(relative);
		REQUIRE_FALSE(fd.has_value());
		CHECK(fd.error().detail == "it leads outside the folder");
	}
	CHECK(contents(dir.path() / "outside.wav") == "keep");
}

// --- read() ---

TEST_CASE("read() gives the text, or nothing when the file is missing", "[folder]") {
	TempDir dir{"read"};
	const auto folder = openFolder(dir.path() / "folder");
	write(dir.path() / "folder" / "ear.conf", "node white noise;\n");
	std::filesystem::create_directory(dir.path() / "folder" / "sub");
	write(dir.path() / "folder" / "sub" / "x.conf", "x");

	CHECK(readText(folder, "ear.conf") == std::optional<std::string>("node white noise;\n"));
	CHECK(readText(folder, "sub/x.conf") == std::optional<std::string>("x"));
	CHECK(readText(folder, "missing.conf") == std::optional<std::string>());
}

TEST_CASE("read() refuses a symlink, a directory, a file too large and any path that leads out", "[folder]") {
	TempDir dir{"readrefuse"};
	const auto folder = openFolder(dir.path() / "folder");
	write(dir.path() / "outside.conf", "outside");
	write(dir.path() / "folder" / "real.conf", "real");
	std::filesystem::create_symlink("real.conf", dir.path() / "folder" / "link.conf");
	std::filesystem::create_directory(dir.path() / "folder" / "sub");
	write(dir.path() / "folder" / "sub" / "x.conf", "x");
	std::filesystem::create_directory_symlink("sub", dir.path() / "folder" / "alias");
	write(dir.path() / "folder" / "big.conf", std::string(maxBytes + 1, 'x'));
	write(dir.path() / "folder" / "exact.conf", std::string(maxBytes, 'x'));

	CHECK(readText(folder, "exact.conf") == std::optional<std::string>(std::string(maxBytes, 'x')));
	for (const std::string& relative :
	     std::vector<std::string>{"link.conf", "sub", "big.conf", "../outside.conf", (dir.path() / "outside.conf").string(), "alias/x.conf"}) {
		INFO(relative);
		const auto read = folder.read(relative, maxBytes);
		REQUIRE_FALSE(read.has_value());
		CHECK(read.error().detail.starts_with(relative + ": "));
	}
	CHECK(folder.read("link.conf", maxBytes).error().detail == "link.conf: a symlink is on its path");
	CHECK(folder.read("sub", maxBytes).error().detail == "sub: not a regular file");
	CHECK(folder.read("big.conf", maxBytes).error().detail == "big.conf: larger than 1024 bytes");
	CHECK(folder.read("../outside.conf", maxBytes).error().detail == "../outside.conf: it leads outside the folder");
}

TEST_CASE("read() refuses a FIFO without waiting for a writer", "[folder]") {
	TempDir dir{"fifo"};
	const auto folder = openFolder(dir.path() / "folder");
	REQUIRE(::mkfifo((dir.path() / "folder" / "pipe.conf").c_str(), 0644) == 0);
	const auto read = folder.read("pipe.conf", maxBytes);
	REQUIRE_FALSE(read.has_value());
	CHECK(read.error().detail == "pipe.conf: not a regular file");
}

// --- replaceIf() ---

TEST_CASE("replaceIf() puts the text in place when the file holds what was expected, keeping its mode",
          "[folder]") {
	TempDir dir{"replace"};
	const auto folder = openFolder(dir.path() / "folder");
	const auto file = dir.path() / "folder" / "ear.conf";
	write(file, "old");
	std::filesystem::permissions(file, std::filesystem::perms(0640));

	const auto outcome = folder.replaceIf("ear.conf", std::string("old"), "new", maxBytes);

	REQUIRE(outcome.has_value());
	CHECK(std::holds_alternative<Replaced>(*outcome));
	CHECK(contents(file) == "new");
	CHECK(permissionsOf(file) == std::filesystem::perms(0640));
	CHECK(names(dir.path() / "folder") == std::vector<std::string>{"ear.conf"});

	// A staging file left behind (a crash mid-write) keeps its own mode when truncated; the target's
	// is still the one that lands.
	const auto leftover = dir.path() / "folder" / ".ear.conf.ear-staging";
	write(leftover, "stale");
	std::filesystem::permissions(leftover, std::filesystem::perms(0600));
	const auto again = folder.replaceIf("ear.conf", std::string("new"), "newer", maxBytes);
	REQUIRE(again.has_value());
	CHECK(std::holds_alternative<Replaced>(*again));
	CHECK(contents(file) == "newer");
	CHECK(permissionsOf(file) == std::filesystem::perms(0640));
	CHECK(names(dir.path() / "folder") == std::vector<std::string>{"ear.conf"});
}

TEST_CASE("replaceIf() leaves a file that holds other text, and says what it holds", "[folder]") {
	TempDir dir{"changed"};
	const auto folder = openFolder(dir.path() / "folder");
	const auto file = dir.path() / "folder" / "ear.conf";
	write(file, "hand edit");

	const auto outcome = folder.replaceIf("ear.conf", std::string("base"), "ear's text", maxBytes);

	REQUIRE(outcome.has_value());
	REQUIRE(std::holds_alternative<Changed>(*outcome));
	CHECK(std::get<Changed>(*outcome).current == std::optional<std::string>("hand edit"));
	CHECK(contents(file) == "hand edit");
	CHECK(names(dir.path() / "folder") == std::vector<std::string>{"ear.conf"});
}

TEST_CASE("replaceIf() on a missing file creates it 0644 only when nothing was expected", "[folder]") {
	TempDir dir{"missing"};
	const auto folder = openFolder(dir.path() / "folder");
	const auto file = dir.path() / "folder" / "ear.conf";

	const auto expectedText = folder.replaceIf("ear.conf", std::string("base"), "new", maxBytes);
	REQUIRE(expectedText.has_value());
	REQUIRE(std::holds_alternative<Changed>(*expectedText));
	CHECK(std::get<Changed>(*expectedText).current == std::nullopt);
	CHECK_FALSE(std::filesystem::exists(file));
	CHECK(names(dir.path() / "folder").empty());

	const auto expectedNothing = folder.replaceIf("ear.conf", std::nullopt, "new", maxBytes);
	REQUIRE(expectedNothing.has_value());
	CHECK(std::holds_alternative<Replaced>(*expectedNothing));
	CHECK(contents(file) == "new");
	CHECK(permissionsOf(file) == std::filesystem::perms(0644));
	CHECK(names(dir.path() / "folder") == std::vector<std::string>{"ear.conf"});
}

TEST_CASE("replaceIf() leaves a file that appeared when nothing was expected", "[folder]") {
	TempDir dir{"appeared"};
	const auto folder = openFolder(dir.path() / "folder");
	const auto file = dir.path() / "folder" / "ear.conf";
	write(file, "hand made");

	const auto outcome = folder.replaceIf("ear.conf", std::nullopt, "ear's text", maxBytes);

	REQUIRE(outcome.has_value());
	REQUIRE(std::holds_alternative<Changed>(*outcome));
	CHECK(std::get<Changed>(*outcome).current == std::optional<std::string>("hand made"));
	CHECK(contents(file) == "hand made");
	CHECK(names(dir.path() / "folder") == std::vector<std::string>{"ear.conf"});
}

TEST_CASE("replaceIf() refuses a symlink target and leaves the symlink", "[folder]") {
	TempDir dir{"symlink"};
	const auto folder = openFolder(dir.path() / "folder");
	write(dir.path() / "folder" / "real.conf", "real");
	const auto link = dir.path() / "folder" / "ear.conf";
	std::filesystem::create_symlink("real.conf", link);

	const auto outcome = folder.replaceIf("ear.conf", std::string("real"), "new", maxBytes);

	REQUIRE_FALSE(outcome.has_value());
	CHECK(outcome.error().detail == "ear.conf: not a regular file");
	CHECK(std::filesystem::is_symlink(link));
	CHECK(std::filesystem::read_symlink(link) == "real.conf");
	CHECK(contents(dir.path() / "folder" / "real.conf") == "real");
	CHECK(names(dir.path() / "folder") == std::vector<std::string>{"ear.conf", "real.conf"});
}

TEST_CASE("replaceIf() refuses a path that leads out or through a symlinked directory", "[folder]") {
	TempDir dir{"replaceout"};
	const auto folder = openFolder(dir.path() / "folder");
	write(dir.path() / "outside.conf", "outside");
	std::filesystem::create_directory_symlink(dir.path(), dir.path() / "folder" / "up");

	for (const std::string& relative : std::vector<std::string>{"../outside.conf", (dir.path() / "outside.conf").string(), "up/outside.conf"}) {
		INFO(relative);
		const auto outcome = folder.replaceIf(relative, std::string("outside"), "new", maxBytes);
		CHECK_FALSE(outcome.has_value());
	}
	CHECK(contents(dir.path() / "outside.conf") == "outside");
	CHECK(names(dir.path()) == std::vector<std::string>{"folder", "outside.conf"});
}

TEST_CASE("replaceIf() in a folder it cannot write is an error and the file is unchanged", "[folder]") {
	if (::geteuid() == 0) {
		WARN("root writes a read-only folder; this case needs an ordinary user");
		return;
	}
	TempDir dir{"readonly"};
	const auto folder = openFolder(dir.path() / "folder");
	const auto file = dir.path() / "folder" / "ear.conf";
	write(file, "old");
	std::filesystem::permissions(dir.path() / "folder", std::filesystem::perms(0555));

	const auto outcome = folder.replaceIf("ear.conf", std::string("old"), "new", maxBytes);

	REQUIRE_FALSE(outcome.has_value());
	CHECK(outcome.error().detail == "ear.conf: cannot stage the new text: Permission denied");
	CHECK(contents(file) == "old");
	CHECK(names(dir.path() / "folder") == std::vector<std::string>{"ear.conf"});
}

// --- heldPath() ---

TEST_CASE("heldPath() names the held folder, and an inotify watch through it sees a rename into it", "[folder]") {
	TempDir dir{"held"};
	const auto folder = openFolder(dir.path() / "folder");
	// The folder is renamed after opening: the held path still names the directory itself.
	std::filesystem::rename(dir.path() / "folder", dir.path() / "moved");

	struct stat held{};
	struct stat real{};
	REQUIRE(::stat(folder.heldPath().c_str(), &held) == 0);
	REQUIRE(::stat((dir.path() / "moved").c_str(), &real) == 0);
	CHECK(held.st_dev == real.st_dev);
	CHECK(held.st_ino == real.st_ino);

	const int watch = ::inotify_init1(IN_CLOEXEC);
	REQUIRE(watch >= 0);
	REQUIRE(::inotify_add_watch(watch, folder.heldPath().c_str(), IN_MOVED_TO) >= 0);
	write(dir.path() / "ear.conf.new", "text");
	std::filesystem::rename(dir.path() / "ear.conf.new", dir.path() / "moved" / "ear.conf");

	// The rename has happened, so its event is already queued; the deadline only fails the test.
	pollfd ready{watch, POLLIN, 0};
	REQUIRE(::poll(&ready, 1, 5000) == 1);
	alignas(inotify_event) char buffer[4096];
	const ssize_t got = ::read(watch, buffer, sizeof buffer);
	REQUIRE(got >= static_cast<ssize_t>(sizeof(inotify_event)));
	const auto* event = reinterpret_cast<const inotify_event*>(buffer);
	CHECK((event->mask & IN_MOVED_TO) != 0);
	CHECK(std::string(event->name) == "ear.conf");
	::close(watch);
}

// --- requireApart() ---

TEST_CASE("requireApart() refuses one folder twice, nesting either way and a symlinked alias", "[folder]") {
	TempDir dir{"apart"};
	const auto a = dir.path() / "a";
	const auto inner = a / "inner";
	const auto alias = dir.path() / "alias";
	std::filesystem::create_directories(inner);
	std::filesystem::create_directory_symlink(a, alias);

	const std::vector<std::vector<std::filesystem::path>> overlapping = {
	    {a, a}, {a, inner}, {inner, a}, {a, alias}, {alias / "inner", a}, {dir.path() / "web", a, alias}};
	std::filesystem::create_directory(dir.path() / "web");
	for (const auto& folders : overlapping) {
		INFO(folders.front().string() << " ... " << folders.back().string());
		const auto checked = ear::folder::requireApart(folders);
		REQUIRE_FALSE(checked.has_value());
		const auto& first = folders.size() == 3 ? folders[1] : folders[0];
		const auto& second = folders.back();
		CHECK(checked.error().detail.find(first.string()) != std::string::npos);
		CHECK(checked.error().detail.find(second.string()) != std::string::npos);
	}
}

TEST_CASE("requireApart() passes folders apart and skips a path that does not exist", "[folder]") {
	TempDir dir{"apartok"};
	std::filesystem::create_directories(dir.path() / "config");
	std::filesystem::create_directories(dir.path() / "recordings");
	std::filesystem::create_directories(dir.path() / "web");
	const std::vector<std::filesystem::path> apart = {dir.path() / "config", dir.path() / "recordings",
	                                                  dir.path() / "web", dir.path() / "missing"};
	CHECK(ear::folder::requireApart(apart).has_value());
	const std::vector<std::filesystem::path> withMissingInside = {dir.path() / "config",
	                                                              dir.path() / "config" / "missing"};
	CHECK(ear::folder::requireApart(withMissingInside).has_value());
	// Siblings whose names share a prefix are apart.
	std::filesystem::create_directories(dir.path() / "config2");
	const std::vector<std::filesystem::path> prefixed = {dir.path() / "config", dir.path() / "config2"};
	CHECK(ear::folder::requireApart(prefixed).has_value());
}
