// WithoutFilePrivileges: a folder's mode binds the test's thread as it binds an ordinary user, whoever runs the tests.
#include <cerrno>
#include <filesystem>
#include <future>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "temp_folder.hpp"
#include "without_file_privileges.hpp"

namespace {

/// errno of creating `path`, or 0 when it was made.
int createError(const std::filesystem::path& path) {
	const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	if (fd < 0) return errno;
	::close(fd);
	std::filesystem::remove(path);
	return 0;
}

} // namespace

TEST_CASE("Without file privileges, a folder's mode binds the thread, and only that thread, while it lasts",
          "[support][privileges]") {
	ear::test::TempFolder dir{"privileges"};
	const auto folder = dir.path() / "readonly";
	std::filesystem::create_directory(folder);
	std::filesystem::permissions(folder, std::filesystem::perms(0555));
	const bool privileged = createError(folder / "before") == 0; // root, or a user with CAP_DAC_OVERRIDE
	CHECK((privileged || ::geteuid() != 0));
	// A thread begun before: a new thread takes its creator's capabilities, so only one already running shows that
	// the guard changes its own thread's alone.
	std::promise<void> go;
	std::future<int> otherThread = std::async(std::launch::async, [&, asked = go.get_future()]() mutable {
		asked.wait();
		return createError(folder / "other");
	});
	{
		const ear::test::WithoutFilePrivileges asAnOrdinaryUser;
		CHECK(createError(folder / "during") == EACCES);
		go.set_value();
		CHECK(otherThread.get() == (privileged ? 0 : EACCES));
	}
	CHECK(createError(folder / "after") == (privileged ? 0 : EACCES));
}
