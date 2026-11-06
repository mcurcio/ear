// The tests' temporary folder (test/support/temp_folder.hpp): removed with everything in it, a
// read-only or search-less subfolder at any depth included, and never reaching through a symlink.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <sys/stat.h>

#include "catch2/catch.hpp"
#include "temp_folder.hpp"

namespace {

using ear::test::TempFolder;
namespace fs = std::filesystem;

std::string contents(const fs::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

fs::perms permissionsOf(const fs::path& path) { return fs::status(path).permissions() & fs::perms::mask; }

} // namespace

TEST_CASE("a temp folder removes a read-only subfolder two levels down", "[temp-folder]") {
	fs::path path;
	{
		TempFolder folder{"removal"};
		path = folder.path();
		const auto deep = path / "a" / "b";
		fs::create_directories(deep / "c");
		std::ofstream(deep / "file.txt") << "x";
		std::ofstream(deep / "c" / "inner.txt") << "y";
		fs::permissions(deep / "c", fs::perms(0555));
		fs::permissions(deep, fs::perms(0500));       // no write: nothing in it can be removed until opened
		fs::permissions(path / "a", fs::perms(0100)); // search only: it cannot even be listed
		REQUIRE(fs::exists(path));
	}
	CHECK_FALSE(fs::exists(path));
}

TEST_CASE("a temp folder leaves what a symlink leads to alone", "[temp-folder]") {
	TempFolder outside{"outside"};
	const auto target = outside.path() / "target";
	fs::create_directory(target);
	std::ofstream(target / "keep.txt") << "keep";
	fs::permissions(target, fs::perms(0555));
	const auto before = permissionsOf(target);
	{
		TempFolder folder{"link"};
		fs::create_directory_symlink(target, folder.path() / "out");
		fs::create_symlink(target / "keep.txt", folder.path() / "out.txt");
	}
	CHECK(permissionsOf(target) == before);
	CHECK(contents(target / "keep.txt") == "keep");
	fs::permissions(target, fs::perms(0755)); // so outside can remove it
}

TEST_CASE("two temp folders differ and are private", "[temp-folder]") {
	TempFolder first{"pair"};
	TempFolder second{"pair"};
	CHECK(first.path() != second.path());
	CHECK(fs::is_directory(first.path()));
	CHECK(permissionsOf(first.path()) == fs::perms(0700));
}
