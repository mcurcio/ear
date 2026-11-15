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
///    its standard output and error pipes and its feed, on a signalfd for SIGCHLD, which main.cpp blocked in every
///    thread before the first started, and on one inotify descriptor (IN_CLOEXEC | IN_NONBLOCK) for /dev/shm; it never
///    sleeps or polls on a timer but programTermBound's.
///  - A launch that needs shared-memory names (ProgramLaunch::needs) first arms, before any file or the spawn, one watch
///    on /dev/shm (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF), shared by every program while
///    any needs a name, and then looks (stat) for each /dev/shm/<name without its "/">; a watch the kernel refuses is
///    refused "cannot watch /dev/shm: <its words>". ready is told once it started and every name is present, at once
///    after started when each already is or it needs none. A name seen present that goes (IN_DELETE, IN_MOVED_FROM)
///    while it runs and was not stopped is recorded as ProgramExit::removed and the program stopped as stop() does,
///    so its exited carries it. A queue overflow (IN_Q_OVERFLOW) looks at every watched name again.
///  - A launch writes the files (0600, O_CLOEXEC | O_EXCL), makes the feed (mkfifo 0600) and opens it O_RDWR |
///    O_NONBLOCK (so its reader never sees end of file while the program restarts its writer), then spawns with
///    pidfd_spawn: the executable through `/usr/bin/setpriv --pdeathsig KILL --nnp --`, so the program dies with this
///    thread (and so with ear) even outside a container and never gains a privilege by a setuid file (its argv[0] is
///    the executable's path, as setpriv executes it); its own process group (POSIX_SPAWN_SETPGROUP); an empty signal
///    mask and every signal at its default (POSIX_SPAWN_SETSIGMASK, POSIX_SPAWN_SETSIGDEF), since ear blocks the stop
///    signals and SIGCHLD in every thread and a child inherits both (every signal, glibc's own two among them, which
///    its sigfillset leaves out and its spawn would leave ignored); descriptors 0 (/dev/null), 1 and 2 (pipes) only
///    (posix_spawn_file_actions_addclosefrom_np(3)); the working folder ProgramLaunch::folder; the environment PATH
///    alone. The executable is checked (access, X_OK) just before the spawn, since setpriv's own failure to execute
///    it would be only an exit status. Any failure removes what it made and tells refused, with the kernel's words.
///  - Lines of standard output and error, each cut at programLineBytes, are told as they end (said); the last line of
///    standard error is kept for exited. Feed bytes are told as read (fed), at most programFeedChunkBytes each.
///  - A pidfd readable is the program's end: SIGKILL to its group (whatever of it is left, sent while the program is
///    a zombie still holding the group's id), then waitid(P_PIDFD) reaps it; what its pipes and its feed still hold is
///    told first (a line the program did not end is told as one), then its files and feed are removed, then exited. A
///    SIGCHLD reaps every other child that ended (waitid(P_ALL, WNOHANG | WNOWAIT) to see it, then a program's through
///    its own pidfd, any other by its pid), so a grandchild whose parent died never stays a zombie.
///  - stop(): SIGTERM to the program's group at once, SIGKILL to it at programTermBound unless it was reaped first, and
///    to what is left of the group at the reaping either way (above).
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
