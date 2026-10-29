// Catch2 runner for the EAR test executable.

#define CATCH_CONFIG_RUNNER
// The vendored Catch 2.12.2 traps with x86 "int $3" on every Mac, which arm64 cannot
// assemble; the clang builtin works on both.
#define CATCH_BREAK_INTO_DEBUGGER() [] { if (Catch::isDebuggerActive()) { __builtin_debugtrap(); } }()
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
