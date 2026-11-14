// A sendspin node's key file (docs/node-types.md#the-identity-file): the store `sendspin-<node>.key` in the graph file
// folder (docs/writing-a-node-type.md#stores), mode 0600, the library's private key, PSKs and pairing records as
// opaque blobs, with their audit fields. The framework opens, narrows, judges (whyNotKeyRecord) and creates it
// (emptyKeyRecord) before the worker runs; this is what the library reads and writes through. Written whole on every
// save, swapped in only if the file is what the store last read or wrote. Holds secrets: never logged or sent anywhere.
#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "ear/core/node_handle.hpp"
#include "ear/core/store.hpp"
#include "sendspin/client.h"
namespace ear::nodes::sendspin {
/// The most a key file holds.
inline constexpr std::uint32_t maxKeyFileBytes = 64 * 1024;
/// The store's initial text: a key record with its audit fields and no blob, created before the library runs, since
/// the library only warns when it cannot save a new key and would run a fresh identity each boot.
std::string emptyKeyRecord();
/// The store's distrust: why `text` is no key record ("malformed: <why>"), or nothing when it is one. A keypair of the
/// wrong size counts: the library would treat it as absent and mint a new identity over trusted state.
std::optional<std::string> whyNotKeyRecord(std::string_view text);
class NodeKeyStore final : public ::sendspin::SendspinPersistenceProvider {
public:
	/// The worker, before client.start(): the record the store opened and trusted. Error: the store's text is no key
	/// record after all (the store's rule judged it: a bug), with why.
	static std::expected<std::unique_ptr<NodeKeyStore>, std::string> open(Store& store, NodeHandle& events);
	std::optional<std::vector<std::uint8_t>> load_blob(const std::string& key) override;
	/// Blocking (the store's replace): the worker's thread or a library thread, never the main loop. Failure:
	/// `key-write-failed`. A write the store could not confirm within its 5 s (StoreWriteError::unconfirmed) is judged
	/// by the store's text: written if it shows this record, else failed, though it may still land. The blob stays in
	/// memory either way, so a later write that works carries it too (the library's contract: this run's state stays
	/// authoritative).
	bool save_blob(const std::string& key, const std::uint8_t* data, std::size_t size) override;

private:
	NodeKeyStore(Store& store, NodeHandle& events, nlohmann::json record) noexcept;
	Store& store_;
	NodeHandle& events_;
	nlohmann::json record_;
};
} // namespace ear::nodes::sendspin
