// The system tests' way to run the real out/ear: spawn it with its standard output and error
// on one pipe, wait for a line it prints, send it a signal and wait for it to exit. Every wait
// is for an event (a line, end of output); its deadline only fails the test.
#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ear::test {

class ChildProcess {
public:
	ChildProcess(const std::string& program, const std::vector<std::string>& arguments) {
		int ends[2];
		if (::pipe2(ends, O_CLOEXEC) != 0) throw std::system_error(errno, std::generic_category(), "pipe2");
		output_fd_ = ends[0];

		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
		posix_spawn_file_actions_adddup2(&actions, ends[1], STDERR_FILENO);

		std::vector<char*> argv;
		argv.push_back(const_cast<char*>(program.c_str()));
		for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
		argv.push_back(nullptr);

		const int spawned = ::posix_spawn(&pid_, program.c_str(), &actions, nullptr, argv.data(), environ);
		posix_spawn_file_actions_destroy(&actions);
		::close(ends[1]);
		if (spawned != 0) {
			::close(output_fd_);
			throw std::system_error(spawned, std::generic_category(), "posix_spawn " + program);
		}
	}

	~ChildProcess() {
		if (pid_ > 0) {
			::kill(pid_, SIGKILL);
			::waitpid(pid_, nullptr, 0);
		}
		if (output_fd_ >= 0) ::close(output_fd_);
	}

	ChildProcess(const ChildProcess&) = delete;
	ChildProcess& operator=(const ChildProcess&) = delete;

	/// Waits until the child has printed `line` as a whole line. False if its output ends or
	/// the deadline passes first.
	bool waitForLine(std::string_view line, std::chrono::milliseconds deadline) {
		const auto until = std::chrono::steady_clock::now() + deadline;
		while (!hasLine(line)) {
			if (!readMore(until)) return false;
		}
		return true;
	}

	void signal(int signalNumber) { ::kill(pid_, signalNumber); }

	/// Waits for the child's output to end and the child to exit. Its exit status, or nothing
	/// if it was killed by a signal or the deadline passed first.
	std::optional<int> waitForExit(std::chrono::milliseconds deadline) {
		const auto until = std::chrono::steady_clock::now() + deadline;
		while (readMore(until)) {
		}
		if (!ended_) return std::nullopt;
		int status = 0;
		if (::waitpid(pid_, &status, 0) != pid_) return std::nullopt;
		pid_ = -1;
		if (!WIFEXITED(status)) return std::nullopt;
		return WEXITSTATUS(status);
	}

	/// Everything the child printed so far, standard output and error interleaved.
	const std::string& output() const noexcept { return output_; }

	bool hasLine(std::string_view line) const {
		std::size_t start = 0;
		while (true) {
			const std::size_t end = output_.find('\n', start);
			if (end == std::string::npos) return false;
			if (std::string_view(output_).substr(start, end - start) == line) return true;
			start = end + 1;
		}
	}

private:
	/// Reads what the child printed next. False once its output ended or `until` passed.
	bool readMore(std::chrono::steady_clock::time_point until) {
		if (ended_) return false;
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
		if (left.count() <= 0) return false;
		pollfd ready{output_fd_, POLLIN, 0};
		const int polled = ::poll(&ready, 1, static_cast<int>(left.count()));
		if (polled < 0 && errno == EINTR) return true;
		if (polled <= 0) return false;
		char buffer[4096];
		const ssize_t got = ::read(output_fd_, buffer, sizeof buffer);
		if (got < 0 && errno == EINTR) return true;
		if (got <= 0) {
			ended_ = true;
			return false;
		}
		output_.append(buffer, static_cast<std::size_t>(got));
		return true;
	}

	pid_t pid_ = -1;
	int output_fd_ = -1;
	bool ended_ = false;
	std::string output_;
};

} // namespace ear::test
