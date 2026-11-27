// The reach index of one version of ear.conf (docs/graph-file.md#reaches): for each node and key of its type, where
// the file defines the key's value, the text of the lines that define it, every pair sharing that definition, the
// value, and the row's facts a gesture's begin is judged by. Made on the keeper's thread from a model and read on the
// main loop: plain values only. And the related-change rule that compares two of them for an open gesture.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "ear/control/graph_file_state.hpp"
#include "ear/core/gestures.hpp"
#include "ear/core/settings_table.hpp"
namespace ear::config {
/// A key's row as a gesture's begin judges it.
struct ReachRow {
	setting::Change change;
	bool ramped;  // live with a ramp (rampMs above 0)
	bool decimal; // a decimal rule
	std::string unit;
	double low = 0.0;
	double high = 0.0;
	unsigned decimals = 0;
	std::optional<std::uint32_t> member; // its slot among the type's live members; a live member's only
	bool active = true;                  // a family member at or below its count, or a plain row
};
struct ReachEntry {
	GestureKey at;
	std::string type; // the node's type
	ReachRow row;
	/// The defining value's byte offset in the text; none when the file does not give the key (it reads its default).
	std::optional<std::int64_t> definition;
	control::TextPlace place{1, 1}; // the definition's, or the node's when not given
	/// The defining statement's text from its first token to its end, comments it owns excluded, hashed (FNV-1a 64);
	/// for a key not given, its node's `node` line's.
	std::uint64_t definingLines = 0;
	NodeSettings value; // as the node's settings hold it; the row's default when not given
};
class ReachIndex {
public:
	explicit ReachIndex(std::vector<ReachEntry> entries) : entries_(std::move(entries)) {}
	const std::vector<ReachEntry>& entries() const noexcept { return entries_; }
	const ReachEntry* find(const GestureKey& at) const;
	/// Every pair whose value is defined where `at`'s is, `at` first then text order; only `at` for a key not given.
	std::vector<GestureKey> reachOf(const GestureKey& at) const;

private:
	std::vector<ReachEntry> entries_;
};
/// How a newer version of the file relates to an open gesture's reach, against the version it began on
/// (docs/graph-file.md#reaches).
enum class Relation {
	none,        // nothing of the reach changed
	value,       // a held key's value changed after expansion
	reach,       // its defining lines or the pairs sharing its definition changed, its value did not
	nodeRemoved, // a reached node is gone (removed, renamed, replaced by another type)
	keyRemoved,  // a reached key is no longer the type's, no longer live, or no longer active
};
/// Pure. The first relation found over the reach's pairs, the most severe kind first: a node gone, a key gone, a
/// value changed, else the reach changed.
Relation relationOf(const ReachIndex& before, const ReachIndex& after, const std::vector<GestureKey>& reach);
} // namespace ear::config
