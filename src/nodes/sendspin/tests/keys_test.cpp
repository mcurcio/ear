// A sendspin node's key file on a real folder (docs/node-types.md#the-identity-file).
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/core/log.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/nodes/sendspin.hpp"
#include "key_store.hpp"
#include "temp_folder.hpp"
#include "without_file_privileges.hpp"

namespace {

using ear::sendspin::NodeKeyStore;
using nlohmann::json;

class Heard final : public ear::NodeHandle {
public:
	const ear::NodeName& node() const noexcept override { return name; }
	void log(ear::LogEntry) override {}
	void say(std::string text) override { said.push_back(std::move(text)); }
	void lost(std::string detail) override { said.push_back("LOST " + std::move(detail)); }
	std::vector<std::string> said;
	ear::NodeName name = "kitchen";
};

struct Rig {
	ear::test::TempFolder dir{"keys"}; // first, so it is removed last
	Rig() {
		root = dir.path();
		auto opened = ear::folder::ConfinedFolder::open(root / "config");
		REQUIRE(opened.has_value());
		folder = std::make_shared<const ear::folder::ConfinedFolder>(std::move(*opened));
		file = root / "config" / "sendspin-ma.key";
	}
	std::unique_ptr<NodeKeyStore> open() {
		auto store = NodeKeyStore::open(folder, "ma", heard);
		INFO((store ? std::string("opened") : store.error()));
		REQUIRE(store.has_value());
		return std::move(*store);
	}
	std::string refusal() {
		auto store = NodeKeyStore::open(folder, "ma", heard);
		REQUIRE_FALSE(store.has_value());
		return store.error();
	}
	std::string text() const {
		std::ifstream in(file, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}
	void write(const std::string& text) const { std::ofstream(file, std::ios::binary) << text; }
	std::filesystem::perms mode() const {
		return std::filesystem::symlink_status(file).permissions() & std::filesystem::perms::mask;
	}
	std::filesystem::path root;
	std::filesystem::path file;
	std::shared_ptr<const ear::folder::ConfinedFolder> folder;
	Heard heard;
};

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
	std::vector<std::uint8_t> out;
	for (const int value : values) out.push_back(static_cast<std::uint8_t>(value));
	return out;
}

const std::string oldRecord =
	R"({"createdAt": "2001-02-03T04:05:06Z", "createdBy": "someone", "changedAt": "2001-02-03T04:05:06Z",
 "changedBy": "someone", "blobs": {}})";

} // namespace

TEST_CASE("A key file is created empty at 0600 with its audit fields, before any blob", "[sendspin][keys]") {
	Rig rig;
	const auto store = rig.open();
	REQUIRE(std::filesystem::exists(rig.file));
	CHECK(rig.mode() == std::filesystem::perms(0600));
	const json record = json::parse(rig.text());
	CHECK(record["createdBy"] == "ear");
	CHECK(record["changedBy"] == "ear");
	CHECK(record["createdAt"].get<std::string>().size() == std::string("2026-11-05T22:00:00Z").size());
	CHECK(record["changedAt"] == record["createdAt"]);
	CHECK(record["blobs"].empty());
	CHECK_FALSE(store->load_blob("keypair").has_value());
}

TEST_CASE("Blobs round-trip exact, through memory and through the file", "[sendspin][keys]") {
	Rig rig;
	std::vector<std::uint8_t> everyByte;
	for (int value = 0; value < 256; ++value) everyByte.push_back(static_cast<std::uint8_t>(value));
	const std::vector<std::uint8_t> keypair(32, 0x5A);
	{
		const auto store = rig.open();
		REQUIRE(store->save_blob("keypair", keypair.data(), keypair.size()));
		REQUIRE(store->save_blob("rec_0", everyByte.data(), 7));
		REQUIRE(store->save_blob("rec_0", everyByte.data(), everyByte.size())); // overwritten in place
		CHECK(store->load_blob("rec_0") == everyByte);
	}
	CHECK(rig.mode() == std::filesystem::perms(0600));
	CHECK(rig.heard.said.empty());
	const auto again = rig.open();
	CHECK(again->load_blob("rec_0") == everyByte);
	CHECK(again->load_blob("keypair") == keypair);
	CHECK_FALSE(again->load_blob("rec_1").has_value());
}

TEST_CASE("createdAt and createdBy are set once, and changedAt and changedBy move with a write", "[sendspin][keys]") {
	Rig rig;
	rig.write(oldRecord);
	const auto store = rig.open();
	const auto blob = bytes({9, 8, 7});
	REQUIRE(store->save_blob("rec_order", blob.data(), blob.size()));
	const json record = json::parse(rig.text());
	CHECK(record["createdAt"] == "2001-02-03T04:05:06Z");
	CHECK(record["createdBy"] == "someone");
	CHECK(record["changedAt"] != "2001-02-03T04:05:06Z");
	CHECK(record["changedBy"] == "ear");
}

TEST_CASE("A file already wider than 0600 is 0600 after open, its text unchanged", "[sendspin][keys]") {
	Rig rig;
	const int mode = GENERATE(0644, 0660);
	rig.write(oldRecord);
	std::filesystem::permissions(rig.file, std::filesystem::perms(mode));
	const auto store = rig.open();
	CHECK(rig.mode() == std::filesystem::perms(0600));
	CHECK(rig.text() == oldRecord);
}

TEST_CASE("A file that is no key file fails open with the reason, its text unchanged and its mode "
          "narrowed",
          "[sendspin][keys]") {
	Rig rig;
	const std::vector<std::pair<std::string, std::string>> cases{
		{"{ not json", "sendspin-ma.key: malformed: not JSON"},
		{"", "sendspin-ma.key: malformed: not JSON"},
		{"[]", "sendspin-ma.key: malformed: not a JSON object"},
		{R"({"createdBy": "a", "changedAt": "b", "changedBy": "c", "blobs": {}})",
	     "sendspin-ma.key: malformed: no createdAt"},
		{R"({"createdAt": "a", "createdBy": "a", "changedAt": "b", "changedBy": "c"})",
	     "sendspin-ma.key: malformed: no blobs"},
		{R"({"createdAt": "a", "createdBy": "a", "changedAt": "b", "changedBy": "c", "blobs": {"rec_0": 1}})",
	     "sendspin-ma.key: malformed: blob rec_0 is not text"},
		{R"({"createdAt": "a", "createdBy": "a", "changedAt": "b", "changedBy": "c", "blobs": {"rec_0": "@"}})",
	     "sendspin-ma.key: malformed: blob rec_0 is not base64url"},
		{R"({"createdAt": "a", "createdBy": "a", "changedAt": "b", "changedBy": "c", "blobs": {"keypair": "AAAA"}})",
	     "sendspin-ma.key: malformed: blob keypair has the wrong size"},
	};
	for (const auto& [text, detail] : cases) {
		INFO(text);
		rig.write(text);
		std::filesystem::permissions(rig.file, std::filesystem::perms(0644));
		CHECK(rig.refusal() == detail);
		CHECK(rig.text() == text);
		CHECK(rig.mode() == std::filesystem::perms(0600)); // a refused file may still hold a private key
	}
}

TEST_CASE("A directory, a symlink out and a file over 64 KiB each fail open with the folder's detail",
          "[sendspin][keys]") {
	Rig rig;
	SECTION("a directory") {
		std::filesystem::create_directory(rig.file);
		CHECK(rig.refusal() == "sendspin-ma.key: not a regular file");
		CHECK(std::filesystem::is_directory(rig.file));
	}
	SECTION("a symlink out") {
		const auto outside = rig.root / "outside.key";
		std::ofstream(outside) << oldRecord;
		std::filesystem::create_symlink(outside, rig.file);
		CHECK(rig.refusal() == "sendspin-ma.key: a symlink is on its path");
		CHECK(std::filesystem::is_symlink(rig.file));
		std::ifstream in(outside, std::ios::binary);
		CHECK(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) == oldRecord);
	}
	SECTION("over 64 KiB") {
		const std::string large(64 * 1024 + 1, 'x');
		rig.write(large);
		CHECK(rig.refusal() == "sendspin-ma.key: larger than 65536 bytes");
		CHECK(rig.text() == large);
	}
}

TEST_CASE("A read-only folder fails open with the folder's detail, and later fails a write without "
          "losing the blob",
          "[sendspin][keys]") {
	const ear::test::WithoutFilePrivileges asAnOrdinaryUser; // first, so the rig is gone before they come back
	Rig rig;
	const auto folderPath = rig.root / "config";
	SECTION("nothing there yet") {
		std::filesystem::permissions(folderPath, std::filesystem::perms(0555));
		CHECK(rig.refusal() == "sendspin-ma.key: cannot stage the new text: Permission denied");
		CHECK_FALSE(std::filesystem::exists(rig.file));
	}
	SECTION("a write later") {
		const auto store = rig.open();
		const std::string before = rig.text();
		std::filesystem::permissions(folderPath, std::filesystem::perms(0555));
		const auto blob = bytes({1, 2, 3});
		CHECK_FALSE(store->save_blob("rec_0", blob.data(), blob.size()));
		REQUIRE(rig.heard.said.size() == 1);
		CHECK(rig.heard.said[0] == "key-write-failed sendspin-ma.key: cannot stage the new text: Permission denied");
		CHECK(rig.text() == before);
		CHECK(store->load_blob("rec_0") == blob);

		std::filesystem::permissions(folderPath, std::filesystem::perms(0755));
		const auto next = bytes({4});
		CHECK(store->save_blob("rec_order", next.data(), next.size()));
		const auto again = rig.open();
		CHECK(again->load_blob("rec_0") == blob);
		CHECK(again->load_blob("rec_order") == next);
	}
}

TEST_CASE("A file something else changed or removed stands, and the write fails", "[sendspin][keys]") {
	Rig rig;
	const auto store = rig.open();
	const auto blob = bytes({1});
	SECTION("changed") {
		rig.write(oldRecord);
		CHECK_FALSE(store->save_blob("rec_0", blob.data(), blob.size()));
		CHECK(rig.text() == oldRecord);
	}
	SECTION("removed") {
		std::filesystem::remove(rig.file);
		CHECK_FALSE(store->save_blob("rec_0", blob.data(), blob.size()));
		CHECK_FALSE(std::filesystem::exists(rig.file));
	}
	REQUIRE(rig.heard.said.size() == 1);
	CHECK(rig.heard.said[0] == "key-write-failed sendspin-ma.key: changed by something else");
}

TEST_CASE("A mode widened mid-run does not outlive the next write", "[sendspin][keys]") {
	Rig rig;
	const auto store = rig.open();
	std::filesystem::permissions(rig.file, std::filesystem::perms(0660));
	const auto blob = bytes({1});
	REQUIRE(store->save_blob("rec_0", blob.data(), blob.size()));
	CHECK(rig.mode() == std::filesystem::perms(0600));
}

TEST_CASE("A blob that would take the file over 64 KiB fails the write, and the file stands", "[sendspin][keys]") {
	Rig rig;
	const auto store = rig.open();
	const std::string before = rig.text();
	const std::vector<std::uint8_t> blob(64 * 1024, 7);
	CHECK_FALSE(store->save_blob("rec_0", blob.data(), blob.size()));
	REQUIRE(rig.heard.said.size() == 1);
	CHECK(rig.heard.said[0].starts_with("key-write-failed sendspin-ma.key: larger than 65536 bytes"));
	CHECK(rig.text() == before);
}

TEST_CASE("Making the node type narrows every key file and its staging file, whatever its node or "
          "its text, and nothing else",
          "[sendspin][keys]") {
	Rig rig;
	const auto config = rig.root / "config";
	const auto write = [&](const char* name, int mode) {
		std::ofstream(config / name, std::ios::binary) << "whatever";
		std::filesystem::permissions(config / name, std::filesystem::perms(mode));
	};
	write("sendspin-removed.key", 0660);
	write(".sendspin-removed.key.ear-staging", 0660);
	write("sendspin-malformed.key", 0644);
	write("ear.conf", 0664);
	std::filesystem::create_symlink(config / "ear.conf", config / "sendspin-link.key");

	struct BootLog final : ear::LogSink {
		std::vector<ear::LogEntry> entries;
		void log(const ear::LogEntry& entry) override { entries.push_back(entry); }
	} boot;
	CHECK(ear::nodes::makeSendspinNodeType({.config = rig.folder, .log = &boot}) != nullptr);
	CHECK_THROWS_WITH(ear::nodes::makeSendspinNodeType({}), "sendspin needs the graph file folder");
	CHECK_THROWS_WITH(ear::nodes::makeSendspinNodeType({.config = rig.folder}), "sendspin needs the boot log");
	// Every key file here can be narrowed (a symlink is not one: the folder lists regular files only), so nothing is
	// reported; a failure would be a warn record from "sendspin" (main.cpp's sink), never dropped for want of a sink.
	CHECK(boot.entries.empty());

	const auto modeOf = [&](const char* name) {
		return std::filesystem::symlink_status(config / name).permissions() & std::filesystem::perms::mask;
	};
	CHECK(modeOf("sendspin-removed.key") == std::filesystem::perms(0600));
	CHECK(modeOf(".sendspin-removed.key.ear-staging") == std::filesystem::perms(0600));
	CHECK(modeOf("sendspin-malformed.key") == std::filesystem::perms(0600));
	CHECK(modeOf("ear.conf") == std::filesystem::perms(0664));
}
