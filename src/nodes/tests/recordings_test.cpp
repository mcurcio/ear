// The recordings folder (docs/design/batch2.md section 4.4): made at boot if missing, never its
// parents; a folder it cannot make or open is an error naming it.
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/nodes/recordings.hpp"

namespace {

class TempDir {
public:
	TempDir()
	    : path_(std::filesystem::temp_directory_path() / ("ear_nodes_recordings_" + std::to_string(getpid()))) {
		std::filesystem::remove_all(path_);
		std::filesystem::create_directories(path_);
	}
	~TempDir() { std::filesystem::remove_all(path_); }
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;
	const std::filesystem::path& path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
};

}  // namespace

TEST_CASE("The recordings folder is made if missing and opened if it is there", "[recordings]") {
	TempDir dir;
	const auto path = dir.path() / "recordings";

	const auto made = ear::nodes::RecordingsFolder::open(path);
	REQUIRE(made.has_value());
	CHECK(std::filesystem::is_directory(path));
	CHECK(made->path() == path);

	const auto again = ear::nodes::RecordingsFolder::open(path);
	CHECK(again.has_value());
}

TEST_CASE("The recordings folder is never made with its parents, and a file is not a folder", "[recordings]") {
	TempDir dir;
	const auto deep = dir.path() / "missing" / "recordings";
	const auto missingParent = ear::nodes::RecordingsFolder::open(deep);
	REQUIRE_FALSE(missingParent.has_value());
	CHECK(missingParent.error().find(deep.string()) != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(dir.path() / "missing"));

	const auto file = dir.path() / "plain";
	std::ofstream(file) << "x";
	const auto notFolder = ear::nodes::RecordingsFolder::open(file);
	REQUIRE_FALSE(notFolder.has_value());
	CHECK(notFolder.error().find(file.string()) != std::string::npos);
}
