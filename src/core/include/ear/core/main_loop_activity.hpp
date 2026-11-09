// Where the main loop is (docs/design/node-lifecycle.md section 5.12, D-L9; step L-3b): the call NodeHost is in (a
// flush item, a type's hook) and the node it is for, written before the call and cleared when it returns, so the
// main-loop watchdog (StallWatch, step L-7) can say where a stall began without a lock and without following a pointer
// into a node. One writer, the main thread; any thread reads. A seqlock: the writer never waits, a reader retries.
#pragma once
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
	/// Any thread: "<call> <node>" (or "<call>" with no node), or empty between calls.
	static std::string current();

	/// Sets on construction and clears on destruction (main thread).
	class Scope {
	public:
		Scope(std::string_view call, std::string_view node) noexcept { set(call, node); }
		~Scope() { clear(); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	};
};
} // namespace ear
