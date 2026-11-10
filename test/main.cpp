// The Catch2 runner every EAR test executable links (ear_catch_main, test/CMakeLists.txt).

#define CATCH_CONFIG_RUNNER
// Catch 2.12.2 sizes its signal stack with MINSIGSTKSZ as a constant, which glibc 2.34 and
// newer define as a sysconf() call, so its signal handlers do not compile on current Linux
// (fixed in Catch 2.13.5). Without them a crashing test still fails: ctest runs each test
// case in its own process and reports the signal, it just loses Catch's crash message.
#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include "catch2/catch.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Catch 2.12's console reporter prints a failing test's name as a header whose later lines are indented to just past
// its first ": ", in a text column CATCH_CONFIG_CONSOLE_WIDTH - 1 wide. An indent at or past that width aborts the
// process (an assert in the column) and one column short of it loops forever printing "-", either way before the
// failing check is printed; a few columns more print the rest a character or two a line. So a test program holding a
// name whose first ": " leaves less than kHeaderRoom columns after it refuses to run at all, naming it, whether or not
// anything would fail (docs/testing.md#how-the-suites-are-built).
constexpr std::size_t kHeaderColumn = CATCH_CONFIG_CONSOLE_WIDTH - 1;
constexpr std::size_t kHeaderRoom = 10;

std::vector<std::string> namesTheReporterCannotPrint(const Catch::IConfig& config) {
	std::vector<std::string> bad;
	for (const Catch::TestCase& test : Catch::getAllTestCasesSorted(config)) {
		const std::size_t colon = test.name.find(": ");
		if (colon != std::string::npos && colon + 2 + kHeaderRoom > kHeaderColumn) bad.push_back(test.name);
	}
	return bad;
}

} // namespace

int main(int argc, char* argv[]) {
	Catch::Session session;
	if (const int error = session.applyCommandLine(argc, argv); error != 0) return error;
	const std::vector<std::string> bad = namesTheReporterCannotPrint(session.config());
	if (!bad.empty()) {
		std::cerr << "Catch's console reporter cannot print a failure of these test cases (the first \": \" ends past "
					 "column "
				  << kHeaderColumn - kHeaderRoom << "); rename them:\n";
		for (const std::string& name : bad) std::cerr << "  " << name << '\n';
		return 3;
	}
	return session.run();
}
