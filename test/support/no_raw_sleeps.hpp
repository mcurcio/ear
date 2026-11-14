// Forced into every source of every test target (cmake/RawSleeps.cmake): a test sleeps only through the role calls of
// sleeps.hpp, and tells nothing and goes on where it should fail naming its event (docs/testing.md#sleeps-by-role).
// After those calls and every header that names a raw sleep or one of Catch's go-on macros, the names are poisoned,
// so a raw sleep, a WARN or a CHECK_NOFAIL anywhere later in the source does not compile, however it is spelt.
//
// A poison is lexical: what was read before it compiles (the role calls, the standard library's and Catch's own code),
// and a header read after it that names a poisoned identifier fails. So every header that names one is included here
// first, those of a library only where the target can see it (__has_include): a header that fails after this one is
// added here, never worked around.
#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <thread>

#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#if __has_include("catch2/catch.hpp")
#include "catch2/catch.hpp"
#endif
// Boost's spinlock backs off with nanosleep (Beast and Asio reach it); sendspin-cpp names its log level WARN.
#if __has_include(<boost/core/detail/sp_thread_sleep.hpp>)
#include <boost/core/detail/sp_thread_sleep.hpp>
#endif
#if __has_include(<sendspin/client.h>)
#include <sendspin/client.h>
#endif

#include "sleeps.hpp"

// Poisoning a defined macro warns, and Catch's were expanded wherever they were used above (nowhere).
#undef WARN
#undef CATCH_WARN
#undef CHECK_NOFAIL
#undef CATCH_CHECK_NOFAIL
#pragma GCC poison sleep_for sleep_until sleep usleep nanosleep clock_nanosleep
#pragma GCC poison WARN CATCH_WARN CHECK_NOFAIL CATCH_CHECK_NOFAIL

// poll, ppoll, select and pselect are a test's to wait on descriptors; on none (nullptr for the set) they are a sleep,
// refused by a deleted overload that matches nullptr exactly and is a better match than the real one. It takes nullptr
// alone, not 0 or NULL, so a library's own call with 0 (Asio's select in null_event) still finds the real function.
template <class None>
	requires std::same_as<None, std::nullptr_t>
int poll(None, nfds_t, int) = delete;
template <class None>
	requires std::same_as<None, std::nullptr_t>
int ppoll(None, nfds_t, const timespec*, const sigset_t*) = delete;
template <class None>
	requires std::same_as<None, std::nullptr_t>
int select(int, None, None, None, timeval*) = delete;
template <class None>
	requires std::same_as<None, std::nullptr_t>
int pselect(int, None, None, None, const timespec*, const sigset_t*) = delete;
