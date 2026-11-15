// Programs (docs/writing-a-node-type.md#programs): a process a configuration runs beside its node, asked for in
// configure (CreateOf::program), started at the last readiness step once every companion is ready, its standard output
// and error lines in the node's log, its report feed (a FIFO it writes) read for the type's loop, its end the node's
// failure, and torn down with the configuration: a stop keeps it running. What a type writes against (ProgramPath,
// the parts (part::), ProgramArg, ProgramFile, ProgramShape), the wording of its end (programExitDetail), the paths of
// a configuration's runtime files (runtimePath), and the process side the framework drives (ProgramAdapter: ear_runtime's
// Programs; the harness's fake).
//
// Who uses what: a type the shape and the parts; NodeHost the rest, resolving every part to text before the adapter sees
// it, so the adapter knows no claim, bank or loop. Pure: nothing but the standard library and ear_core.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include "ear/core/claims.hpp"
#include "ear/core/graph_spec.hpp"
#include "ear/core/log.hpp"
namespace ear {

// ---- What a type asks for (CreateOf::program) ----

/// A program's executable: an absolute path written as a constant in the type's code. Its constructor is consteval, so
/// nothing read at run time (a graph file's setting, a peer's text) can name what ear executes: a path that is not
/// absolute, has an empty, "." or ".." component, ends in "/" or holds a character outside [A-Za-z0-9._+/-] does not
/// compile.
class ProgramPath {
public:
	consteval ProgramPath(const char* path) : path_(path) {
		if (!plain(path)) throw "a program's executable is an absolute path of plain components: [A-Za-z0-9._+-]";
	}
	constexpr std::string_view path() const noexcept { return path_; }
	/// The last component: the program's argv[0], its log lines' source and the name its failures give.
	constexpr std::string_view name() const noexcept {
		const std::string_view whole(path_);
		return whole.substr(whole.rfind('/') + 1);
	}

private:
	static consteval bool plain(const char* path) {
		const std::string_view whole(path);
		if (whole.size() < 2 || whole.front() != '/' || whole.back() == '/') return false;
		std::size_t begin = 1;
		while (begin <= whole.size()) {
			const std::size_t end = std::min(whole.find('/', begin), whole.size());
			const std::string_view part = whole.substr(begin, end - begin);
			if (part.empty() || part == "." || part == "..") return false;
			for (const char c : part) {
				const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
				if (!letter && c != '.' && c != '_' && c != '+' && c != '-') return false;
			}
			begin = end + 1;
		}
		return true;
	}
	const char* path_;
};

/// The parts a program's arguments and runtime files are made of. Each resolves to text when the program starts: a
/// type names what the framework hands across (a granted port, a pipe receiver's FIFO, the report feed, a runtime file)
/// without knowing a number or a path.
namespace part {
/// As given, one argument's worth: an argument is never read by a shell, so no character needs escaping in one. A
/// runtime file's syntax is its program's: escaping text into it is the type's.
struct Text {
	std::string text;
	friend bool operator==(const Text&, const Text&) = default;
};
/// The granted port's number, in decimal.
struct Port {
	GrantedPort port;
	friend bool operator==(const Port&, const Port&) = default;
};
/// The path of the FIFO of the pipe receiver this configuration asked for on inside bank `bank` (CreateOf::fifoOf makes
/// one, checked against the spec). A bank with no pipe receiver is a broken rule.
struct Fifo {
	std::string bank;
	friend bool operator==(const Fifo&, const Fifo&) = default;
};
/// The path of the program's report feed: a FIFO the framework makes and holds open for reading before the program
/// starts, whose bytes reach the loop CreateOf::program names. A program asked for with no feed is a broken rule.
struct Feed {
	friend bool operator==(const Feed&, const Feed&) = default;
};
/// The path of the program's runtime file `name` (ProgramShape::files). A name the shape lacks is a broken rule.
struct File {
	std::string name;
	friend bool operator==(const File&, const File&) = default;
};
} // namespace part
using ProgramPart = std::variant<part::Text, part::Port, part::Fifo, part::Feed, part::File>;

/// One argument of a program's command line: its parts' texts joined, so `{part::Text{"--port="}, part::Port{p}}`
/// is one argument.
struct ProgramArg {
	ProgramArg(const char* text);
	ProgramArg(std::string text);
	ProgramArg(ProgramPart piece);
	ProgramArg(std::initializer_list<ProgramPart> parts);
	std::vector<ProgramPart> parts;
	friend bool operator==(const ProgramArg&, const ProgramArg&) = default;
};

/// A program's runtime file: written beside the configuration's other runtime files (mode 0600) before the program
/// starts, and removed once it is reaped.
struct ProgramFile {
	std::string name;              // 1 to 32 of [a-z0-9.-], unique among the program's files; never "feed"
	std::vector<ProgramPart> text; // its content: the parts' texts joined (a Fifo, Feed or File names a path)
	friend bool operator==(const ProgramFile&, const ProgramFile&) = default;
};

/// A program a configuration asks for (CreateOf::program). Its environment is PATH alone
/// (`/usr/local/bin:/usr/bin:/bin`), its working folder ear's runtime folder, its standard input /dev/null.
struct ProgramShape {
	ProgramPath executable;
	std::vector<ProgramArg> args{};   // argv[1] onwards; argv[0] is executable.name()
	std::vector<ProgramFile> files{}; // at most 8
};

/// How long a program may take to start, from its asking (the files written, the feed opened, the process spawned).
/// Past it the node fails start-failed "<name> did not start within 5 s" (docs/engine.md#readiness-deadlines).
inline constexpr std::chrono::milliseconds programReadinessBound{5'000};
/// How long a program has to end after its teardown's SIGTERM before its process group is sent SIGKILL. Inside the
/// close deadline, so a program that ignores SIGTERM is reaped before the configuration would be leaked.
inline constexpr std::chrono::milliseconds programTermBound{3'000};
/// The longest line of a program's output a log entry carries; a longer one is cut there (LogEntry::cut).
inline constexpr std::size_t programLineBytes = 1'024;
/// The most bytes one delivery of a report feed carries (one read of the FIFO).
inline constexpr std::size_t programFeedChunkBytes = 4'096;

// ---- Its end, and its files' paths ----

/// How a program ended, as its reaping told.
struct ProgramExit {
	enum class How { exited, killed };
	How how = How::exited;
	int number = 0;         // the exit status (0-255), or the signal that killed it
	std::string lastLine{}; // its last line on standard error, cut at programLineBytes; empty: none
	friend bool operator==(const ProgramExit&, const ProgramExit&) = default;
};

/// "<name> exited with status <n>" or "<name> was killed by signal <n> (SIG<abbrev>)", then ": <last line>" when it has
/// one: the one home of the words of a program's end, which its node fails with (start-failed before the node first
/// rested running or stopped, lost after).
std::string programExitDetail(std::string_view name, const ProgramExit& exit);

/// `<folder>/<node>.<configuration>.<what>`: the one home of a configuration's runtime file paths (a pipe receiver's
/// FIFO, `<bank>.fifo`; a program's feed, `feed`; its files, by name). `configuration` is NodeHost's count of the
/// configurations it made, so no two configurations, a leaked one and its successor among them, share a path, and no
/// claim is needed for one. A node's name has no "." or "/", so a path is one node's alone.
std::string runtimePath(std::string_view folder, const NodeName& node, std::uint64_t configuration,
                        std::string_view what);

// ---- The process side (ear_runtime's Programs implements it) ----

/// A program as NodeHost hands it over: every part resolved to text, every path beneath ear's runtime folder.
struct ProgramLaunch {
	NodeName node;                                            // whose: its log lines' node
	std::string executable;                                   // ProgramPath::path()
	std::vector<std::string> argv;                            // argv[0] is the executable's name
	std::vector<std::pair<std::string, std::string>> files{}; // path, text: each written 0600 before it starts
	std::optional<std::string> feed{}; // a FIFO to make (0600) and hold open for reading before it starts
	std::string folder;                // its working folder: ear's runtime folder
};

/// What a program tells, on the adapter's own thread; NodeHost's implementation only crosses to the main loop (through
/// the configuration's channel: docs/architecture.md#teardown-by-dependency) or to the log sinks.
class ProgramEvents {
public:
	/// It runs: its files written, its feed open, its process spawned (exec succeeded). At most once.
	virtual void started() = 0;
	/// It cannot run (a file, the feed or the spawn failed): `why`, the cause in the kernel's words. At most once,
	/// instead of started; nothing follows it, and whatever it made is already removed.
	virtual void refused(std::string why) = 0;
	/// Bytes of its report feed, in order, at most programFeedChunkBytes each; only after started.
	virtual void fed(std::string bytes) = 0;
	/// One line of its standard output (level info) or error (warn): `source` its name, `topic` "stdout" or "stderr",
	/// `node` set, cut at programLineBytes. Only after started.
	virtual void said(LogEntry entry) = 0;
	/// It was reaped: how it ended. At most once, after started; its files and feed are already removed, and the
	/// adapter calls nothing more.
	virtual void exited(ProgramExit exit) = 0;

protected:
	~ProgramEvents() = default;
};

/// One launched program. Main thread.
class RunningProgram {
public:
	/// Precondition: refused or exited was told, so nothing of it runs; never waits on the adapter's thread.
	virtual ~RunningProgram() = default;
	/// Its teardown's first step: SIGTERM to its process group, then SIGKILL to it past programTermBound unless it was
	/// reaped first; exited follows. Again, or after it ended, is nothing.
	virtual void stop() = 0;
};

/// The processes under the nodes' programs (ResourceAdapters::programs). The process's part only: when a program
/// starts and stops is NodeHost's. A std::logic_error from launch is its bug (docs/architecture.md#faults-by-owner).
class ProgramAdapter {
public:
	/// Main thread; never blocks: the work runs on the adapter's thread, which then tells `events` (outliving the
	/// RunningProgram) started or refused.
	virtual std::unique_ptr<RunningProgram> launch(ProgramLaunch launch, ProgramEvents& events) = 0;

protected:
	~ProgramAdapter() = default;
};

} // namespace ear
