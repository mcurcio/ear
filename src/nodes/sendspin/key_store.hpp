// A sendspin node's key file (docs/design/sendspin.md section 8): `sendspin-<node>.key` in the graph file folder, mode
// 0600, the library's private key, PSKs and pairing records as opaque blobs, with their audit fields. Written whole on
// every save, swapped in only if the file is what it last read or wrote. Holds secrets: never logged or sent anywhere.
#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "ear/core/node_type.hpp"
#include "ear/folder/confined_folder.hpp"
#include "sendspin/client.h"
namespace ear::sendspin {
/// Boot. Every `sendspin-*.key` in `folder` and each one's staging file a crash left, narrowed to 0600 whatever its node and
/// whatever it holds: an fsGroup widens them on every mount, and nothing else opens the file of a node no longer in the
/// graph (section 8). What it could not narrow, one detail each.
std::vector<std::string> narrowKeyFiles(const folder::ConfinedFolder& folder);
class NodeKeyStore final : public ::sendspin::SendspinPersistenceProvider {
public:
	/// Worker, before client.start(). Reads and narrows to 0600, or creates it empty. Error: `lost` with this detail, and
	/// the file's text left as it was.
	static std::expected<std::unique_ptr<NodeKeyStore>, std::string>
	open(std::shared_ptr<const folder::ConfinedFolder> folder, const NodeName& node, InsidesEvents& events);
	std::optional<std::vector<std::uint8_t>> load_blob(const std::string& key) override;
	/// Failure: `key-write-failed`. The blob stays in memory, so a later write that works carries it too (the library's
	/// contract: this run's state stays authoritative).
	bool save_blob(const std::string& key, const std::uint8_t* data, std::size_t size) override;

private:
	NodeKeyStore(std::shared_ptr<const folder::ConfinedFolder> folder, std::string file, InsidesEvents& events,
	             nlohmann::json record, std::string text) noexcept;
	std::shared_ptr<const folder::ConfinedFolder> folder_;
	std::string file_;
	InsidesEvents& events_;
	nlohmann::json record_;
	std::string text_; // what the file held when last read or written
};
} // namespace ear::sendspin
