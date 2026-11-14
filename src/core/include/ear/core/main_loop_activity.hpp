// Where the main loop is (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog): the call NodeHost is in
// (a flush item, a type's hook) and the node it is for, written before the call and cleared when it returns, so the
// main-loop watchdog (StallWatch) can say where a stall began without a lock and without following a pointer into a
// node. One writer, the main thread; any thread reads. A seqlock: the writer never waits, a reader retries (bounded).
// Scopes nest: one inside another (a hook called inside a flush item) names the inner call while it runs and gives the
// outer one back when it ends. A throw that ends a scope leaves the scope's place behind (takeThrown), so the callback
// boundary that catches it, after every scope has unwound, can still say where it was thrown
// (docs/architecture.md#the-callback-boundary).
#pragma once
#include <array>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
namespace ear {
class MainLoopActivity {
public:
	/// The longest node name kept, in bytes; a longer one is cut there.
	static constexpr std::size_t nameBytes = 96;
	/// Main thread. `call` names the call and must be a string literal (only its address is kept); `node` is copied.
	static void set(std::string_view call, std::string_view node) noexcept;
	/// Main thread: between calls.
	static void clear() noexcept;
	/// Any thread: "<call> <node>" (or "<call>" with no node), or empty between calls. Yields between tries and, if
	/// the writer is mid-write for 1,000 of them, gives up and says empty (unknown).
	static std::string current();

	/// What the main thread last wrote, kept to be written again (Scope).
	struct Saved {
		const char* call = nullptr;
		std::size_t callSize = 0;
		std::array<char, nameBytes> name{};
		std::size_t nameSize = 0;
	};
	/// Main thread: what is written now.
	static Saved save() noexcept;
	/// Main thread: writes `saved` again.
	static void restore(const Saved& saved) noexcept;
	/// "<call> <node>" (or "<call>") of a saved place, as current() says it; empty for one between calls.
	static std::string describe(const Saved& saved);

	/// Main thread: where the throw being handled left the main loop: the innermost scope it ended, kept until it is
	/// taken or forgotten; nothing when it ended no scope. Taking it forgets it. Only core's dispatchers open scopes
	/// and put places back (NodeHost, and the scope an adapter opens around telling core, as Links does its route
	/// link listeners), so a place kept says the throw came out of core: the callback boundary takes it as core's bug,
	/// never the adapter's whose callback it came out of (docs/architecture.md#faults-by-owner).
	static std::optional<Saved> takeThrown() noexcept;
	/// Main thread: a catch that keeps an exception to rethrow later puts the place it took back just before the
	/// rethrow, so the place travels with the exception (no scope the rethrow ends replaces it).
	static void putThrown(const Saved& at) noexcept;
	/// Main thread: no throw's place is kept. A scope that begins, and the callback boundary when a main-loop callback
	/// begins, forget the place of a throw that was handled before them.
	static void forgetThrown() noexcept;

	/// Sets on construction and, on destruction, gives back what was written before it (main thread), so an inner
	/// scope ends naming the outer call again, and the outermost ends between calls. One ended by a throw keeps its
	/// place for takeThrown unless an inner scope the same throw ended already did. A scope that begins, or ends by
	/// returning, forgets a place kept before (that throw was handled), unless an exception is unwinding: a scope
	/// opened and closed inside a destructor that the unwinding runs must leave the place the exception carries.
	class Scope {
	public:
		Scope(std::string_view call, std::string_view node) noexcept
			: previous_(save()), uncaught_(std::uncaught_exceptions()) {
			if (uncaught_ == 0) forgetThrown();
			set(call, node);
		}
		~Scope() {
			const int uncaught = std::uncaught_exceptions();
			if (uncaught > uncaught_) endedByThrow();
			else if (uncaught == 0) forgetThrown();
			restore(previous_);
		}
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		static void endedByThrow() noexcept;
		Saved previous_;
		int uncaught_;
	};
};
} // namespace ear
