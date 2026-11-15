// A process that launches a program through Programs and is then killed, for reaper_test.cpp's parent-death check:
// it prints "launcher <its pid>", launches /bin/sh, which prints its pid and becomes /bin/sleep (so the pid is the
// sleep's), prints "sleep <that pid>", and then waits for good. When the test SIGKILLs it, the sleep must die too:
// setpriv gave it SIGKILL as its parent-death signal, sent when the thread that spawned it (ear-programs) dies.
#include <cstdio>
#include <future>
#include <string>

#include <unistd.h>

#include "ear/runtime/programs.hpp"

namespace {

void print(const std::string& line) {
	const std::string text = line + "\n";
	[[maybe_unused]] const ssize_t wrote = ::write(1, text.data(), text.size());
}

class Relay final : public ear::ProgramEvents {
public:
	void started() override {}
	void ready() override {}
	void refused(std::string why) override { print("refused " + why); }
	void fed(std::string) override {}
	void said(ear::LogEntry entry) override {
		if (entry.topic == "stdout") print("sleep " + entry.text);
	}
	void exited(ear::ProgramExit) override { print("sleep ended"); }
};

} // namespace

int main() {
	print("launcher " + std::to_string(::getpid()));
	ear::runtime::Programs programs;
	Relay relay;
	const auto sleeper = programs.launch(ear::ProgramLaunch{.node = "launcher",
	                                                        .executable = "/bin/sh",
	                                                        .argv = {"sh", "-c", "echo \"$$\"; exec /bin/sleep 600"},
	                                                        .folder = "/"},
	                                     relay);
	// Until the test kills it: a future nothing will fulfil.
	std::promise<void> never;
	never.get_future().wait();
	return 0;
}
