#include "key_store.hpp"

#include <chrono>
#include <format>
#include <string_view>
#include <utility>
#include <variant>

#include <sys/stat.h>

#include "notes.hpp"
#include "sendspin/persistence_codec.h"

namespace ear::sendspin {
namespace {

using nlohmann::json;

constexpr std::size_t maxFileBytes = 64 * 1024;
constexpr mode_t fileMode = 0600;
constexpr std::string_view writer = "ear";

std::string now() {
	return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string render(const json& record) { return record.dump(2) + '\n'; }

/// Why `text` is no key file, or nothing when it is one. A keypair of the wrong size counts: the library would treat it
/// as absent and mint a new identity over trusted state.
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
		if (key == ::sendspin::persistence_keys::KEYPAIR && bytes->size() != ::sendspin::persistence_keys::KEYPAIR_SIZE) {
			return std::format("blob {} has the wrong size", key);
		}
	}
	return std::nullopt;
}

} // namespace

std::vector<std::string> narrowKeyFiles(const folder::ConfinedFolder& folder) {
	std::vector<std::string> failures;
	auto names = folder.fileNames();
	if (!names) return {names.error().detail};
	for (const std::string& name : *names) {
		const bool keyFile = name.starts_with("sendspin-") && name.ends_with(".key");
		const bool staged = name.starts_with(".sendspin-") && name.ends_with(".key.ear-staging");
		if (!keyFile && !staged) continue;
		if (auto narrowed = folder.restrictMode(name, fileMode); !narrowed) failures.push_back(narrowed.error().detail);
	}
	return failures;
}

std::expected<std::unique_ptr<NodeKeyStore>, std::string>
NodeKeyStore::open(std::shared_ptr<const folder::ConfinedFolder> folder, const NodeName& node, InsidesEvents& events) {
	const std::string file = "sendspin-" + node + ".key";
	auto read = folder->read(file, maxFileBytes);
	if (!read) return std::unexpected(read.error().detail);

	json record;
	std::string text;
	if (*read) {
		// Every open, because an fsGroup widens the file on every mount, and before its text is judged, because a file
		// refused for its text may still hold a private key (section 8).
		if (auto narrowed = folder->restrictMode(file, fileMode); !narrowed) return std::unexpected(narrowed.error().detail);
		text = std::move(**read);
		record = json::parse(text, nullptr, false);
		if (record.is_discarded()) return std::unexpected(file + ": malformed: not JSON");
		if (const auto why = whyMalformed(record)) return std::unexpected(file + ": malformed: " + *why);
	} else {
		// Now, because the library only warns when it cannot save a new key, and would run a fresh identity each boot.
		const std::string at = now();
		record = {{"createdAt", at}, {"createdBy", writer}, {"changedAt", at}, {"changedBy", writer}, {"blobs", json::object()}};
		text = render(record);
		auto created = folder->replaceIf(file, std::nullopt, text, maxFileBytes, fileMode);
		if (!created) return std::unexpected(created.error().detail);
		if (!std::holds_alternative<folder::Replaced>(*created)) return std::unexpected(file + ": appeared while being created");
	}
	return std::unique_ptr<NodeKeyStore>(new NodeKeyStore(std::move(folder), file, events, std::move(record), std::move(text)));
}

NodeKeyStore::NodeKeyStore(std::shared_ptr<const folder::ConfinedFolder> folder, std::string file, InsidesEvents& events,
                           nlohmann::json record, std::string text) noexcept
    : folder_(std::move(folder)), file_(std::move(file)), events_(events), record_(std::move(record)), text_(std::move(text)) {}

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
	if (text.size() > maxFileBytes) {
		events_.say(notes::keyWriteFailed(std::format("{}: larger than {} bytes", file_, maxFileBytes)));
		return false;
	}
	// Staged with the mode of what it replaces, so narrowed first: a mode widened mid-run must not outlive this write.
	if (const auto narrowed = folder_->restrictMode(file_, fileMode); !narrowed) {
		const auto there = folder_->read(file_, maxFileBytes);
		const bool missing = there && !*there;
		events_.say(notes::keyWriteFailed(missing ? file_ + ": changed by something else" : narrowed.error().detail));
		return false;
	}
	const auto replaced = folder_->replaceIf(file_, text_, text, maxFileBytes, fileMode);
	if (!replaced) {
		events_.say(notes::keyWriteFailed(replaced.error().detail));
		return false;
	}
	if (!std::holds_alternative<folder::Replaced>(*replaced)) {
		// Someone changed or removed it: theirs stands (section 11: deleting it and restarting wipes the identity).
		events_.say(notes::keyWriteFailed(file_ + ": changed by something else"));
		return false;
	}
	text_ = std::move(text);
	return true;
}

} // namespace ear::sendspin
