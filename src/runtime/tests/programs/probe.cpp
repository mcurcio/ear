// What a program finds when it starts, printed one fact a line for programs_test.cpp: its environment ("env <entry>"),
// its working folder ("cwd <path>"), what its standard input is ("stdin <target>"), each descriptor it holds ("fd <n>",
// leaving out the one it reads /proc/self/fd through) and its signal mask and ignored set (/proc/self/status's SigBlk and
// SigIgn lines). A compiled program, not a script, so no shell's own descriptors or signals stand in the way.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <dirent.h>
#include <limits.h>
#include <unistd.h>

extern char** environ;

int main() {
	for (char** entry = environ; *entry != nullptr; ++entry) std::printf("env %s\n", *entry);
	char cwd[PATH_MAX];
	if (::getcwd(cwd, sizeof cwd) != nullptr) std::printf("cwd %s\n", cwd);
	char target[PATH_MAX];
	const ssize_t length = ::readlink("/proc/self/fd/0", target, sizeof target - 1);
	if (length >= 0) {
		target[length] = '\0';
		std::printf("stdin %s\n", target);
	}
	DIR* fds = ::opendir("/proc/self/fd");
	if (fds == nullptr) return 1;
	const int own = ::dirfd(fds);
	while (const dirent* entry = ::readdir(fds)) {
		const std::string name = entry->d_name;
		if (name == "." || name == ".." || std::atoi(name.c_str()) == own) continue;
		std::printf("fd %s\n", name.c_str());
	}
	::closedir(fds);
	std::ifstream status("/proc/self/status");
	for (std::string line; std::getline(status, line);)
		if (line.starts_with("SigBlk:") || line.starts_with("SigIgn:")) std::printf("%s\n", line.c_str());
	return 0;
}
