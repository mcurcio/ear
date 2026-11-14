// A sendspin node's key file (docs/node-types.md#the-identity-file): the key record the store is created with and
// trusted as, the blobs the library reads and writes through it, how a write's outcome is told (key-write-failed), and
// the boot sweep over the type's declared store. How a store is opened, narrowed, judged and written is the framework's
// (core's store_test.cpp, ear_runtime's stores_test.cpp); the node's failure on a distrusted file, type_test.cpp's.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "catch2/catch.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/nodes/sendspin.hpp"
#include "ear/runtime/stores.hpp"
#include "key_store.hpp"
#include "temp_folder.hpp"

namespace {

using ear::nodes::sendspin::NodeKeyStore;
using nlohmann::json;

class Heard final : public ear::NodeHandle {
public:
	const ear::NodeName& node() const noexcept override { return name; }
	void log(ear::LogEntry) override {}
	void say(std::string text) override { said.push_back(std::move(text)); }
	void lost(std::string detail) override { said.push_back("LOST " + std::move(detail)); }
	std::vector<std::string> said;
	ear::NodeName name = "ma";
};

/// A store whose writes go as the test scripts them, its text as the framework keeps it: the last write that committed.
class ScriptedStore final : public ear::Store {
public:
	/// How the next replace ends: committed, refused with `detail`, or unconfirmed (its 5 s passed while it was under
	/// way) having `landed` or not by the time the caller looks.
	struct Next {
		enum class Kind { committed, refused, unconfirmed } kind = Kind::committed;
		std::string detail{};
		bool landed = false;
	};
	explicit ScriptedStore(std::string text) : text_(std::move(text)) {}
	const std::string& fileName() const noexcept override { return name_; }
	std::string text() const override { return text_; }
	ear::StoreWritten replace(std::string text) override {
		++writes;
		const Next now = std::exchange(next, Next{});
		switch (now.kind) {
		case Next::Kind::committed:
			text_ = std::move(text);
			return {};
		case Next::Kind::refused:
			return std::unexpected(ear::StoreWriteError{ear::StoreWriteError::Kind::refused, now.detail});
		case Next::Kind::unconfirmed:
			if (now.landed) text_ = std::move(text);
			return std::unexpected(ear::StoreWriteError{ear::StoreWriteError::Kind::unconfirmed, now.detail});
		}
		return {};
	}
	void replaceLater(std::string, ear::Loop&, std::move_only_function<void(ear::StoreWritten)>) override {
		FAIL("the key store writes with replace");
	}
	Next next;
	int writes = 0;

private:
	std::string name_ = "sendspin-ma.key";
	std::string text_;
};

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
	std::vector<std::uint8_t> out;
	for (const int value : values) out.push_back(static_cast<std::uint8_t>(value));
	return out;
}

const std::string oldRecord =
	R"({"createdAt": "2001-02-03T04:05:06Z", "createdBy": "someone", "changedAt": "2001-02-03T04:05:06Z",
 "changedBy": "someone", "blobs": {}})";

std::unique_ptr<NodeKeyStore> opened(ear::Store& store, Heard& heard) {
	auto keys = NodeKeyStore::open(store, heard);
	INFO((keys ? std::string("opened") : keys.error()));
	REQUIRE(keys.has_value());
	return std::move(*keys);
}

} // namespace

TEST_CASE("A key file is created as a record with its audit fields and no blob, and is trusted", "[sendspin][keys]") {
	const std::string text = ear::nodes::sendspin::emptyKeyRecord();
	CHECK_FALSE(ear::nodes::sendspin::whyNotKeyRecord(text).has_value());
	const json record = json::parse(text);
	CHECK(record["createdBy"] == "ear");
	CHECK(record["changedBy"] == "ear");
	CHECK(record["createdAt"].get<std::string>().size() == std::string("2026-11-05T22:00:00Z").size());
	CHECK(record["changedAt"] == record["createdAt"]);
	CHECK(record["blobs"].empty());
	ScriptedStore store(text);
	Heard heard;
	CHECK_FALSE(opened(store, heard)->load_blob("keypair").has_value());
}

TEST_CASE("A text that is no key record is distrusted with the reason", "[sendspin][keys]") {
	const std::vector<std::pair<std::string, std::string>> cases{
		{"{ not json", "malformed: not JSON"},
		{"", "malformed: not JSON"},
		{"[]", "malformed: not a JSON object"},
		{R"({"createdBy": "a", "changedAt": "b", "changedBy": "c", "blobs": {}})", "malformed: no createdAt"},
		{R"({"createdAt": "a", "createdBy": "a", "changedAt": "b", "changedBy": "c"})", "malformed: no blobs"},
		{R"({"createdAt": "a", "createdBy": "a", "changedAt": "b", "changedBy": "c", "blobs": {"rec_0": 1}})",
	     "malformed: blob rec_0 is not text"},
		{R"({"createdAt": "a", "createdBy": "a", "changedAt": "b", "changedBy": "c", "blobs": {"rec_0": "@"}})",
	     "malformed: blob rec_0 is not base64url"},
		{R"({"createdAt": "a", "createdBy": "a", "changedAt": "b", "changedBy": "c", "blobs": {"keypair": "AAAA"}})",
	     "malformed: blob keypair has the wrong size"},
	};
	for (const auto& [text, detail] : cases) {
		INFO(text);
		CHECK(ear::nodes::sendspin::whyNotKeyRecord(text) == detail);
	}
	CHECK_FALSE(ear::nodes::sendspin::whyNotKeyRecord(oldRecord).has_value());
}

TEST_CASE("Blobs round-trip exact, through memory and through the store's text", "[sendspin][keys]") {
	ScriptedStore store(ear::nodes::sendspin::emptyKeyRecord());
	Heard heard;
	std::vector<std::uint8_t> everyByte;
	for (int value = 0; value < 256; ++value) everyByte.push_back(static_cast<std::uint8_t>(value));
	const std::vector<std::uint8_t> keypair(32, 0x5A);
	{
		const auto keys = opened(store, heard);
		REQUIRE(keys->save_blob("keypair", keypair.data(), keypair.size()));
		REQUIRE(keys->save_blob("rec_0", everyByte.data(), 7));
		REQUIRE(keys->save_blob("rec_0", everyByte.data(), everyByte.size())); // overwritten in place
		CHECK(keys->load_blob("rec_0") == everyByte);
	}
	CHECK(heard.said.empty());
	CHECK(store.writes == 3);
	const auto again = opened(store, heard);
	CHECK(again->load_blob("rec_0") == everyByte);
	CHECK(again->load_blob("keypair") == keypair);
	CHECK_FALSE(again->load_blob("rec_1").has_value());
}

TEST_CASE("createdAt and createdBy are set once, and changedAt and changedBy move with a write", "[sendspin][keys]") {
	ScriptedStore store(oldRecord);
	Heard heard;
	const auto keys = opened(store, heard);
	const auto blob = bytes({9, 8, 7});
	REQUIRE(keys->save_blob("rec_order", blob.data(), blob.size()));
	const json record = json::parse(store.text());
	CHECK(record["createdAt"] == "2001-02-03T04:05:06Z");
	CHECK(record["createdBy"] == "someone");
	CHECK(record["changedAt"] != "2001-02-03T04:05:06Z");
	CHECK(record["changedBy"] == "ear");
}

TEST_CASE("A refused write says key-write-failed with the store's detail, and the blob it carried stays for the next",
          "[sendspin][keys]") {
	ScriptedStore store(oldRecord);
	Heard heard;
	const auto keys = opened(store, heard);
	store.next = {.kind = ScriptedStore::Next::Kind::refused, .detail = "sendspin-ma.key: changed by something else"};
	const auto blob = bytes({1, 2, 3});
	CHECK_FALSE(keys->save_blob("rec_0", blob.data(), blob.size()));
	CHECK(heard.said == std::vector<std::string>{"key-write-failed sendspin-ma.key: changed by something else"});
	CHECK(store.text() == oldRecord);
	CHECK(keys->load_blob("rec_0") == blob);

	const auto next = bytes({4});
	CHECK(keys->save_blob("rec_order", next.data(), next.size()));
	const auto again = opened(store, heard);
	CHECK(again->load_blob("rec_0") == blob);
	CHECK(again->load_blob("rec_order") == next);
}

TEST_CASE("A write the store could not confirm is judged by its text: written if the text shows it, failed if not",
          "[sendspin][keys]") {
	ScriptedStore store(oldRecord);
	Heard heard;
	const auto keys = opened(store, heard);
	const auto blob = bytes({5, 6});
	const std::string unconfirmed =
		"sendspin-ma.key: not confirmed within 5 s: the write was under way and may still land (compare text())";
	SECTION("it landed: written, nothing said") {
		store.next = {.kind = ScriptedStore::Next::Kind::unconfirmed, .detail = unconfirmed, .landed = true};
		CHECK(keys->save_blob("rec_0", blob.data(), blob.size()));
		CHECK(heard.said.empty());
		CHECK(opened(store, heard)->load_blob("rec_0") == blob);
	}
	SECTION("not shown yet: failed, saying so, the blob kept for the next write") {
		store.next = {.kind = ScriptedStore::Next::Kind::unconfirmed, .detail = unconfirmed, .landed = false};
		CHECK_FALSE(keys->save_blob("rec_0", blob.data(), blob.size()));
		CHECK(heard.said == std::vector<std::string>{"key-write-failed " + unconfirmed});
		CHECK(keys->load_blob("rec_0") == blob);
	}
}

TEST_CASE("The boot sweep narrows every sendspin key file and its staging file, whatever its node or its text, and "
          "nothing else",
          "[sendspin][keys]") {
	const ear::test::TempFolder dir{"keys"};
	const auto config = dir.path();
	const auto write = [&](const char* name, int mode) {
		std::ofstream(config / name, std::ios::binary) << "whatever";
		std::filesystem::permissions(config / name, std::filesystem::perms(mode));
	};
	write("sendspin-removed.key", 0660);
	write(".sendspin-removed.key.ear-staging", 0660);
	write("sendspin-malformed.key", 0644);
	write("ear.conf", 0664);
	auto folder = ear::folder::ConfinedFolder::open(config);
	REQUIRE(folder.has_value());
	std::vector<std::unique_ptr<const ear::NodeType>> types;
	types.push_back(ear::nodes::makeSendspinNodeType());
	CHECK(ear::runtime::sweepStores(*folder, types).empty());

	const auto modeOf = [&](const char* name) {
		return std::filesystem::symlink_status(config / name).permissions() & std::filesystem::perms::mask;
	};
	CHECK(modeOf("sendspin-removed.key") == std::filesystem::perms(0600));
	CHECK(modeOf(".sendspin-removed.key.ear-staging") == std::filesystem::perms(0600));
	CHECK(modeOf("sendspin-malformed.key") == std::filesystem::perms(0600));
	CHECK(modeOf("ear.conf") == std::filesystem::perms(0664));
}
