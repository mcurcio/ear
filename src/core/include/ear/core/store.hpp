// A node's stores (docs/writing-a-node-type.md#stores): small files that belong to one node and outlive its
// configurations (a private key, a stream's identity), declared in its spec, named by the framework
// "<type>-<node>.<suffix>" in the graph file folder, opened and judged before any of the node's loop work runs, kept no
// wider than their mode, and replaced only if unchanged. What a type declares and writes against (store::file, Stores,
// Store) and what the framework opens and writes a store through (StoreAdapter: ear_runtime's ConfinedStores implements
// it over the graph file folder and the I/O thread, the harness in memory). The policy (when a file is trusted, what
// is narrowed when, what a write may replace) is NodeHost's; an adapter only does the disk's part. Pure: nothing but
// the standard library and ear_core.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include "ear/core/io_executor.hpp"
#include "ear/core/loop.hpp"
#include "ear/core/store_rule.hpp"
namespace ear {

/// The most a store may hold.
inline constexpr std::uint32_t maxStoreBytes = 1024 * 1024;
/// A store's open is a readiness item with this bound (docs/engine.md#readiness-deadlines); a blocking replace waits
/// this long.
inline constexpr std::chrono::milliseconds storeBound{5'000};

namespace spec_detail {
/// A store's suffix as a template argument, so each store of a spec is its own type.
template <std::size_t Size> struct StoreSuffix {
	char text[Size]{};
	consteval StoreSuffix(const char (&name)[Size]) { // NOLINT: implicit on purpose, from a string literal
		std::copy_n(name, Size, text);
	}
	constexpr std::string_view view() const noexcept { return {text, Size - 1}; }
};
/// 1 to 16 of [a-z0-9-]: the end of a file name, "<type>-<node>.<suffix>".
consteval bool validStoreSuffix(std::string_view suffix) {
	return !suffix.empty() && suffix.size() <= 16 && std::ranges::all_of(suffix, [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
	});
}
// Never defined: a store::file whose rule breaks one of these calls it, so the compiler refuses the spec naming the
// rule broken (a constant expression cannot call a function that is not constexpr).
void a_store_gives_its_maxBytes();
void a_store_holds_at_most_1_MiB();
void a_store_mode_has_no_bits_outside_0777();
} // namespace spec_detail

/// One store of a type, as a compile-time constant: its suffix (its type) and its rule (its value). Made only by
/// store::file.
template <spec_detail::StoreSuffix Suffix> struct StoreHandle {
	static_assert(spec_detail::validStoreSuffix(Suffix.view()), "a store's suffix is 1 to 16 of [a-z0-9-]");
	static constexpr std::string_view suffix = Suffix.view();
	StoreRule rule;
};
template <class T> inline constexpr bool isStoreHandle = false;
template <spec_detail::StoreSuffix Suffix> inline constexpr bool isStoreHandle<StoreHandle<Suffix>> = true;

namespace store {
/// A store "<type>-<node>.<suffix>": inline constexpr auto key = store::file<"key">({.maxBytes = 64 * 1024}). A rule
/// with no maxBytes, more than 1 MiB, or a mode with bits outside 0777 does not compile.
template <spec_detail::StoreSuffix Suffix> consteval StoreHandle<Suffix> file(StoreRule rule) {
	if (rule.maxBytes == 0) spec_detail::a_store_gives_its_maxBytes();
	if (rule.maxBytes > maxStoreBytes) spec_detail::a_store_holds_at_most_1_MiB();
	if ((rule.mode & ~std::uint32_t{0777}) != 0) spec_detail::a_store_mode_has_no_bits_outside_0777();
	return StoreHandle<Suffix>{rule};
}
} // namespace store

/// A spec's stores (TypeSpec's `.stores`), in declaration order, the order the framework opens them. Their suffixes are
/// unique.
template <auto... Handles>
	requires(isStoreHandle<std::remove_cvref_t<decltype(Handles)>> && ...)
struct Stores {
	static constexpr std::size_t count = sizeof...(Handles);
	static_assert(
		[] {
			constexpr std::array<std::string_view, count> names{decltype(Handles)::suffix...};
			for (std::size_t at = 0; at < count; ++at)
				for (std::size_t earlier = 0; earlier < at; ++earlier)
					if (names[at] == names[earlier]) return false;
			return true;
		}(),
		"a type's stores have unique suffixes");
	/// Whether `Handle` is one of these stores.
	template <class Handle> static constexpr bool has =
		(std::same_as<Handle, std::remove_cvref_t<decltype(Handles)>> || ...);
	/// Where `Handle` sits among these stores; only for a store `has` holds for.
	template <class Handle> static constexpr std::size_t indexOf = [] {
		constexpr std::array<bool, count> same{std::same_as<Handle, std::remove_cvref_t<decltype(Handles)>>...};
		return static_cast<std::size_t>(std::ranges::find(same, true) - same.begin());
	}();
	static constexpr std::array<StoreDeclaration, count> declared{
		StoreDeclaration{decltype(Handles)::suffix, Handles.rule}...};
};
template <class T> inline constexpr bool isStores = false;
template <auto... Handles> inline constexpr bool isStores<Stores<Handles...>> = true;

/// Why a write did not commit, or might not have. `refused`: it did not, and the file is as it was. `unconfirmed`: its
/// wait gave up (5 s) while the write was under way on the I/O thread, so it may still land: never read it as "not
/// written"; compare text(), which shows it once it landed, or write with replaceLater, whose outcome is exact.
struct StoreWriteError {
	enum class Kind { refused, unconfirmed };
	Kind kind;
	std::string detail; // a sentence naming the file
};
/// A write's outcome.
using StoreWritten = std::expected<void, StoreWriteError>;

/// "<type>-<node>.<suffix>", a store's file in the graph file folder.
std::string storeFileName(std::string_view type, std::string_view node, std::string_view suffix);

/// A store as a type sees it (cx.store(handle)): valid until the configuration is freed (teardown step 5), open and
/// trusted once the node is configured, so from a loop's handler, start() and a sink consumer's begin on. Before its
/// open completed, text() and replace() throw std::logic_error (a type bug: the node fails).
class Store {
public:
	/// "<type>-<node>.<suffix>".
	virtual const std::string& fileName() const noexcept = 0;
	/// Any thread: what the file held when it was opened and trusted (or was created with; empty when it is missing
	/// and the rule has no initial text), then the text of the last write that committed.
	virtual std::string text() const = 0;
	/// Blocking, so never from a strand (asserted in debug builds), the main loop (std::logic_error: a hook uses
	/// replaceLater) or the realtime thread: a dedicated loop's handler or a library's thread. Queued on the I/O thread
	/// behind the store's earlier writes: narrows the file's mode, then swaps `text` in only if the file still holds
	/// text(). Waits at most 5 s. Refused ("<file>: changed by something else", "<file>: larger than <n> bytes",
	/// "<file>: not written: not begun within 5 s", or the disk's own): the file is as it was. Unconfirmed: the write
	/// was under way when the 5 s passed and may still land (StoreWriteError).
	virtual StoreWritten replace(std::string text) = 0;
	/// The same without waiting, from any thread: `done` runs on `loop` (one of the node's own) with the exact outcome
	/// (never unconfirmed), or never once the loop closed or the configuration ended.
	virtual void replaceLater(std::string text, Loop& loop, std::move_only_function<void(StoreWritten)> done) = 0;

protected:
	~Store() = default;
};

/// What a replaceIf found: it held what was expected and now holds the new text, or it held something else (or
/// nothing) and is left as it was.
enum class StoreWrite { replaced, changed };
/// How a job a caller waited for ended: it ran; its bound passed first (it may still run); or it was destroyed unrun
/// (the I/O thread had stopped).
enum class JobWait { ran, overdue, dropped };

/// The disk under the nodes' stores (ResourceAdapters::stores): ear_runtime's ConfinedStores, over the graph file
/// folder (every name resolved beneath it, no symlink anywhere on the path) and the I/O thread. It does the disk's part
/// only; NodeHost decides what is read, narrowed, judged and written, and in what order. A std::logic_error from one of
/// its calls is its bug (docs/architecture.md#faults-by-owner): the node fails lost, ear.internal-error.
class StoreAdapter {
public:
	/// Where every store's open and writes run, one at a time in post order; outlives every store.
	virtual IoExecutor& io() noexcept = 0;
	/// The calling thread waits until `work`, posted to io() as `job`, ran, or `bound` passed, or it was destroyed
	/// unrun. Core reads no clock of its own, so the waiting is the adapter's.
	virtual JobWait await(std::string job, std::move_only_function<void()> work, std::chrono::milliseconds bound) = 0;
	// io()'s thread, blocking. Each error is a sentence naming the file.
	/// The regular file's text; nothing when it is missing. A symlink, a directory or more than maxBytes is an error.
	virtual std::expected<std::optional<std::string>, std::string> read(const std::string& file,
	                                                                    std::uint32_t maxBytes) = 0;
	/// read(), with every mode bit outside `mode` cleared first on the same descriptor, so the file narrowed is the file
	/// read (a rename between two opens cannot make one file judged and another narrowed); a file too large to read
	/// is still narrowed.
	virtual std::expected<std::optional<std::string>, std::string>
	readNarrowed(const std::string& file, std::uint32_t maxBytes, std::uint32_t mode) = 0;
	/// Clears every mode bit outside `mode`. A missing file is an error.
	virtual std::expected<void, std::string> narrow(const std::string& file, std::uint32_t mode) = 0;
	/// Puts `text` in place only if the file holds `expected` (nothing: no file), atomically (a staging file renamed
	/// over it); a file it replaces keeps its mode, one it creates has exactly `mode`.
	virtual std::expected<StoreWrite, std::string> replaceIf(const std::string& file,
	                                                         const std::optional<std::string>& expected,
	                                                         std::string_view text, std::uint32_t maxBytes,
	                                                         std::uint32_t mode) = 0;

protected:
	~StoreAdapter() = default;
};

} // namespace ear
