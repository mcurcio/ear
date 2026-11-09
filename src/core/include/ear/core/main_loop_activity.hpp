// Where the main loop is (docs/design/node-lifecycle.md section 5.12, D-L9; step L-3b): the call NodeHost is in (a
// flush item, a type's hook) and the node it is for, written before the call and cleared when it returns, so the
// main-loop watchdog (StallWatch, step L-7) can say where a stall began without a lock and without following a pointer
// into a node. One writer, the main thread; any thread reads. A seqlock: the writer never waits, a reader retries
// (bounded).
// Scopes nest (step L-3c): one inside another (a hook called inside a flush item) names the inner call while it runs and
// gives the outer one back when it ends.
#pragma once
#include <array>
#include <cstddef>
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

	/// Sets on construction and, on destruction, gives back what was written before it (main thread), so an inner
	/// scope ends naming the outer call again, and the outermost ends between calls.
	class Scope {
	public:
		Scope(std::string_view call, std::string_view node) noexcept : previous_(save()) { set(call, node); }
		~Scope() { restore(previous_); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		Saved previous_;
	};
};
} // namespace ear
