// The processes under the nodes' programs (docs/writing-a-node-type.md#programs): core's ProgramAdapter, on one thread
// of its own, `ear-programs`, which spawns, watches and reaps every program and every orphan ear inherits. The process's
// part only: when a program starts and stops is NodeHost's.
#pragma once
#include <memory>
#include "ear/core/program.hpp"
namespace ear::runtime {

/// Every program ear runs, and the reaping of every process re-parented to ear (docs/architecture.md#boot-and-stop: ear
/// is its descendants' subreaper, PID 1 in its image).
///  - Its thread starts with the first launch and stops in the destructor. It waits in epoll on each program's pidfd,
///    its standard output and error pipes and its feed, and on a signalfd for SIGCHLD, which main.cpp blocked in every
///    thread before the first started; it never sleeps or polls on a timer but programTermBound's.
///  - A launch writes the files (0600, O_CLOEXEC | O_EXCL), makes the feed (mkfifo 0600) and opens it O_RDWR |
///    O_NONBLOCK (so its reader never sees end of file while the program restarts its writer), then spawns with
///    pidfd_spawn: the executable through `/usr/bin/setpriv --pdeathsig KILL --`, so the program dies with this thread
///    (and so with ear) even outside a container; its own process group (POSIX_SPAWN_SETPGROUP); an empty signal mask
///    and every signal at its default (POSIX_SPAWN_SETSIGMASK, POSIX_SPAWN_SETSIGDEF), since ear blocks the stop signals
///    and SIGCHLD in every thread and a child inherits both; descriptors 0 (/dev/null), 1 and 2 (pipes) only
///    (posix_spawn_file_actions_addclosefrom_np(3)); the working folder ProgramLaunch::folder; the environment PATH
///    alone. Any failure removes what it made and tells refused, with the kernel's words.
///  - Lines of standard output and error, each cut at programLineBytes, are told as they end (said); the last line of
///    standard error is kept for exited. Feed bytes are told as read (fed), at most programFeedChunkBytes each.
///  - A pidfd readable is the program's end: waitid(P_PIDFD) reaps it; what its pipes still hold is told first, then
///    its files and feed are removed, then exited. A SIGCHLD reaps every other child that ended (waitid(P_ALL,
///    WNOHANG | WNOWAIT) to see it, then a program's through its own pidfd, any other by its pid), so a grandchild
///    whose parent died never stays a zombie.
///  - stop(): SIGTERM to the program's group at once, SIGKILL to it at programTermBound unless it was reaped first.
///    Destroying a RunningProgram (after exited or refused) forgets it; never waits on the thread beyond a lock.
class Programs final : public ProgramAdapter {
public:
	Programs();
	/// Precondition: every program was told exited or refused (every node's teardown ended). Stops the thread.
	~Programs();
	Programs(const Programs&) = delete;
	Programs& operator=(const Programs&) = delete;
	std::unique_ptr<RunningProgram> launch(ProgramLaunch launch, ProgramEvents& events) override;

	struct Shared; // the thread's state, which each RunningProgram's record holds by shared_ptr

private:
	std::shared_ptr<Shared> shared_;
};

} // namespace ear::runtime
