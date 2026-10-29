// Catch2 runner for the EAR test executable.

#define CATCH_CONFIG_RUNNER
// The vendored Catch 2.12.2 traps with x86 "int $3" on every Mac, which arm64 cannot
// assemble; the clang builtin works on both. Catch's Linux trap already fits each
// architecture (int $3 on x86, raise(SIGTRAP) elsewhere), and GCC has no __builtin_debugtrap.
#ifdef __APPLE__
#define CATCH_BREAK_INTO_DEBUGGER() [] { if (Catch::isDebuggerActive()) { __builtin_debugtrap(); } }()
#endif
// Catch 2.12.2 sizes its signal stack with MINSIGSTKSZ as a constant, which glibc 2.34 and
// newer define as a sysconf() call, so its signal handlers do not compile on current Linux
// (fixed in Catch 2.13.5). Without them a crashing test still fails: ctest runs each test
// case in its own process and reports the signal, it just loses Catch's crash message.
#ifdef __linux__
#define CATCH_CONFIG_NO_POSIX_SIGNALS
#endif
#include "catch2/catch.hpp"

#include "juce_events/juce_events.h"

int main(int argc, char* argv[]) {
	const int result = Catch::Session().run(argc, argv);

	// The audio graph creates JUCE's MessageManager singleton on first use (the test thread
	// becomes the message thread); release it so JUCE's leak checks stay quiet.
	juce::DeletedAtShutdown::deleteAll();
	juce::MessageManager::deleteInstance();

	return result;
}
