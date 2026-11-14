// The nodes' recordings on real files (ear/runtime/recordings.hpp; docs/writing-a-node-type.md#recordings):
// ConfinedRecordings' disk part (created or truncated beneath the folder only, a descriptor closed when its file goes)
// and its I/O executor. When a recording is claimed, opened and closed is core's (node_host_test.cpp).
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/runtime/io_thread.hpp"
#include "ear/runtime/recordings.hpp"
#include "temp_folder.hpp"

namespace {

using ear::runtime::ConfinedRecordings;
using ear::test::TempFolder;

struct NoFaults final : ear::InternalErrorListener {
	void internalError(const ear::InternalError& error) override {
		FAIL("a fault: " << error.where << ": " << error.what);
	}
};

std::shared_ptr<const ear::folder::ConfinedFolder> opened(const std::filesystem::path& path) {
	auto folder = ear::folder::ConfinedFolder::open(path);
	REQUIRE(folder);
	return std::make_shared<const ear::folder::ConfinedFolder>(std::move(*folder));
}

} // namespace

TEST_CASE("ConfinedRecordings truncates or creates a file beneath its folder, its descriptor closed with the file",
          "[runtime][recordings]") {
	TempFolder dir{"recordings"};
	{
		std::ofstream old(dir.path() / "take.wav");
		old << "old text";
	}
	NoFaults faults;
	ear::runtime::IoThread io(faults);
	ConfinedRecordings recordings(opened(dir.path()), io);
	CHECK(&recordings.io() == &io);

	auto made = recordings.open("rec", "take.wav");
	REQUIRE(made);
	const int fd = (*made)->descriptor();
	CHECK(std::filesystem::file_size(dir.path() / "take.wav") == 0);
	REQUIRE(::write(fd, "new", 3) == 3);
	made->reset();
	CHECK(::fcntl(fd, F_GETFD) == -1); // closed
	CHECK(std::filesystem::file_size(dir.path() / "take.wav") == 3);

	auto fresh = recordings.open("rec", "fresh.wav");
	REQUIRE(fresh);
	CHECK(std::filesystem::exists(dir.path() / "fresh.wav"));
}

TEST_CASE("ConfinedRecordings refuses a path that leads outside its folder or into a folder that is missing, naming no "
          "path",
          "[runtime][recordings]") {
	TempFolder dir{"outside"};
	std::filesystem::create_directories(dir.path() / "recordings");
	std::filesystem::create_directory_symlink(dir.path(), dir.path() / "recordings" / "up");
	NoFaults faults;
	ear::runtime::IoThread io(faults);
	ConfinedRecordings recordings(opened(dir.path() / "recordings"), io);
	const auto out = recordings.open("rec", "up/x.wav");
	REQUIRE_FALSE(out);
	CHECK(out.error() == "it leads outside the folder");
	CHECK_FALSE(std::filesystem::exists(dir.path() / "x.wav"));
	const auto missing = recordings.open("rec", "missing/x.wav");
	REQUIRE_FALSE(missing);
	CHECK_FALSE(missing.error().empty());
}
