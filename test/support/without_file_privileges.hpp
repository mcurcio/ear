// A test's thread without the privileges that let root past a file's mode (header-only; the target ear_temp_folder puts
// it on the include path): a case that needs a folder it cannot write runs as an ordinary user would, under root too,
// instead of skipping (docs/testing.md#an-ordinary-users-folder).
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include <linux/capability.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ear::test {

/// For its lifetime, the calling thread lacks CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH and CAP_FOWNER in its effective set,
/// so a folder's mode binds it as it binds an ordinary user; they come back (from the permitted set, which keeps them)
/// when it goes. The raw capset changes the calling thread alone (glibc's set*id calls would change every thread), so
/// threads already running keep theirs; one begun while it lasts starts without them, as a thread takes its creator's.
/// For an ordinary user it changes nothing. Throws std::runtime_error, which
/// Catch reports as the test's failure, when the thread's capabilities cannot be read or changed: the case then fails
/// naming why, never runs as root.
class WithoutFilePrivileges {
public:
	WithoutFilePrivileges() {
		if (::syscall(SYS_capget, &header_, saved_) != 0) fail("capget");
		__user_cap_data_struct lowered[_LINUX_CAPABILITY_U32S_3];
		std::memcpy(lowered, saved_, sizeof lowered);
		for (const int capability : {CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH, CAP_FOWNER})
			lowered[CAP_TO_INDEX(capability)].effective &= ~CAP_TO_MASK(capability);
		if (::syscall(SYS_capset, &header_, lowered) != 0) fail("capset");
	}
	/// A thread left without them would run every later case as an ordinary user, unseen: that ends the run instead.
	~WithoutFilePrivileges() {
		if (::syscall(SYS_capset, &header_, saved_) != 0) {
			std::fprintf(stderr, "WithoutFilePrivileges: capset could not restore the test thread's capabilities: %s\n",
			             std::strerror(errno));
			std::abort();
		}
	}
	WithoutFilePrivileges(const WithoutFilePrivileges&) = delete;
	WithoutFilePrivileges& operator=(const WithoutFilePrivileges&) = delete;

private:
	[[noreturn]] static void fail(const char* call) {
		throw std::runtime_error(std::string(call) + " on the test's thread failed: " + std::strerror(errno) +
		                         "; this case needs a folder's mode to bind it");
	}
	__user_cap_header_struct header_{_LINUX_CAPABILITY_VERSION_3, 0};
	__user_cap_data_struct saved_[_LINUX_CAPABILITY_U32S_3]{};
};

} // namespace ear::test
