// The Catch2 runner every EAR test executable links (ear_catch_main, test/CMakeLists.txt).

#define CATCH_CONFIG_MAIN
// Catch 2.12.2 sizes its signal stack with MINSIGSTKSZ as a constant, which glibc 2.34 and
// newer define as a sysconf() call, so its signal handlers do not compile on current Linux
// (fixed in Catch 2.13.5). Without them a crashing test still fails: ctest runs each test
// case in its own process and reports the signal, it just loses Catch's crash message.
#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include "catch2/catch.hpp"
