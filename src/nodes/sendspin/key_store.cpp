#include "key_store.hpp"

#include <chrono>
#include <format>
#include <utility>

#include "sendspin/persistence_codec.h"

namespace ear::nodes::sendspin {
namespace {

using nlohmann::json;

constexpr std::string_view writer = "ear";

std::string now() {
	return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string render(const json& record) { return record.dump(2) + '\n'; }

/// Why `record` is no key record, or nothing when it is one.
std::optional<std::string> whyMalformed(const json& record) {
	if (!record.is_object()) return "not a JSON object";
	for (const char* field : {"createdAt", "createdBy", "changedAt", "changedBy"}) {
		if (!record.contains(field) || !record[field].is_string()) return std::format("no {}", field);
	}
	if (!record.contains("blobs") || !record["blobs"].is_object()) return "no blobs";
	for (const auto& [key, blob] : record["blobs"].items()) {
		if (!blob.is_string()) return std::format("blob {} is not text", key);
		const auto bytes = ::sendspin::base64url_decode(blob.get_ref<const std::string&>());
		if (!bytes) return std::format("blob {} is not base64url", key);
		if (key == ::sendspin::persistence_keys::KEYPAIR &&
		    bytes->size() != ::sendspin::persistence_keys::KEYPAIR_SIZE) {
			return std::format("blob {} has the wrong size", key);
		}
	}
	return std::nullopt;
}

} // namespace

std::string emptyKeyRecord() {
	const std::string at = now();
	return render({{"createdAt", at},
	               {"createdBy", writer},
	               {"changedAt", at},
	               {"changedBy", writer},
	               {"blobs", json::object()}});
}

std::optional<std::string> whyNotKeyRecord(std::string_view text) {
	const json record = json::parse(text, nullptr, false);
	if (record.is_discarded()) return "malformed: not JSON";
	if (const auto why = whyMalformed(record)) return "malformed: " + *why;
	return std::nullopt;
}

std::expected<std::unique_ptr<NodeKeyStore>, std::string> NodeKeyStore::open(Store& store, WriteFailed writeFailed) {
	const std::string text = store.text();
	if (const auto why = whyNotKeyRecord(text)) return std::unexpected(store.fileName() + ": " + *why);
	return std::unique_ptr<NodeKeyStore>(new NodeKeyStore(store, std::move(writeFailed), json::parse(text)));
}

NodeKeyStore::NodeKeyStore(Store& store, WriteFailed writeFailed, nlohmann::json record) noexcept
	: store_(store), writeFailed_(std::move(writeFailed)), record_(std::move(record)) {}

std::optional<std::vector<std::uint8_t>> NodeKeyStore::load_blob(const std::string& key) {
	const json& blobs = record_["blobs"];
	const auto found = blobs.find(key);
	if (found == blobs.end()) return std::nullopt;
	return ::sendspin::base64url_decode(found->get_ref<const std::string&>());
}

bool NodeKeyStore::save_blob(const std::string& key, const std::uint8_t* data, std::size_t size) {
	record_["blobs"][key] = ::sendspin::base64url_encode(data, size);
	record_["changedAt"] = now();
	record_["changedBy"] = writer;
	std::string text = render(record_);
	const StoreWritten written = store_.replace(text);
	if (written) return true;
	// Unconfirmed is neither success nor failure (docs/writing-a-node-type.md#stores): the write was under way and may
	// still land, and the store's text shows it once it has. Not shown yet, it is told as failed, the pairing holding
	// for this run as for any failed write; if it lands later, the file holds this run's state all the same.
	if (written.error().kind == StoreWriteError::Kind::unconfirmed && store_.text() == text) return true;
	// Refused: someone changed or removed it, and theirs stands (deleting it and restarting wipes the identity:
	// docs/node-types.md#the-identity-file); or the disk refused.
	writeFailed_(written.error().detail);
	return false;
}

} // namespace ear::nodes::sendspin
