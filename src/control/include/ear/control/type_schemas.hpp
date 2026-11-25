// The node types' schemas on the wire, feature `types` (docs/control-port.md#type-schemas): types.get's answer, encoded
// once at boot from every type's NodeType::schema(), and its hash, which the Graph carries as `typeSchemaHash`.
#pragma once
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include "ear/control/commands.hpp"
#include "ear/control/rpc.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/node_type.hpp"
namespace ear::control {
/// Every type's schema as types.get sends it, and its hash: made once, at boot. An empty one (no hash) is a port made
/// without types, as some tests make.
struct TypeSchemas {
	std::string hash;                                     // "fnv1a64:<16 hex>" of typeSchemas.dump()
	nlohmann::json typeSchemas = nlohmann::json::array(); // one object per type, in the order given
};
/// One type's schema: {"name", "settings": [row...], "inputs", "outputs", "reconfigureMs"}.
nlohmann::json encodeTypeSchema(const TypeSchema& schema);
/// The schemas in the order given, and their hash.
TypeSchemas encodeTypeSchemas(std::span<const TypeSchema> schemas);
/// Every type of the catalogue, in name order (the Graph's `types`).
TypeSchemas encodeTypeSchemas(const NodeCatalogue& catalogue);
/// FNV-1a 64 of `bytes`, written "fnv1a64:<16 lowercase hex>": the one home of the hash's text.
std::string fnv1a64(std::string_view bytes);
/// types.get's params: absent, {} or {"ifNoneMatch": <string>}; the hash it names, if any. A missing, extra or mistyped
/// member is invalidParams with data.pointer, as every decoder's.
std::expected<std::optional<std::string>, RpcError> decodeGetTypes(const nlohmann::json& params);
/// types.get's result: {"protocol", "hash", "typeSchemas"}, or {"protocol", "hash", "notModified": true} when
/// `ifNoneMatch` is the hash.
nlohmann::json typesResult(const TypeSchemas& schemas, const std::optional<std::string>& ifNoneMatch);
} // namespace ear::control
