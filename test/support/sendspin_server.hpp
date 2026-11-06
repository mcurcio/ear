// The tests' way to run the sendspin test server (sendspin_server.py), the real SendspinServer in the image's venv:
// spawn it, send it a command per line, wait for an event it prints. Every wait is for an event; its deadline only fails
// the test. Standard error stays the test's. Header-only, used by ear_sendspin's clock tests and the system tests.
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

class SendspinTestServer {
public:
	/// `arguments` follow `--listen 0`. Returns once it is ready to be dialled.
	explicit SendspinTestServer(const std::vector<std::string>& arguments = {}) {
		int in[2];
		int out[2];
		if (::pipe2(in, O_CLOEXEC) != 0 || ::pipe2(out, O_CLOEXEC) != 0) {
			throw std::system_error(errno, std::generic_category(), "pipe2");
		}
		toServer_ = in[1];
		fromServer_ = out[0];
		// A server gone while a command is on its way shows as an event never coming, not as SIGPIPE here.
		std::signal(SIGPIPE, SIG_IGN);

		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
		posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
		std::vector<std::string> command{EAR_SENDSPIN_SERVER_PYTHON, "-I", EAR_SENDSPIN_SERVER_SCRIPT, "--listen", "0"};
		command.insert(command.end(), arguments.begin(), arguments.end());
		std::vector<char*> argv;
		for (std::string& word : command) argv.push_back(word.data());
		argv.push_back(nullptr);
		const int spawned = ::posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
		posix_spawn_file_actions_destroy(&actions);
		::close(in[0]);
		::close(out[1]);
		if (spawned != 0) {
			pid_ = -1;
			throw std::system_error(spawned, std::generic_category(), "posix_spawn the sendspin test server");
		}
		const auto ready = waitForEvent("ready ", std::chrono::seconds(30));
		if (!ready) throw std::runtime_error("the sendspin test server never said ready");
		url_ = ready->substr(6);
	}

	~SendspinTestServer() {
		if (toServer_ >= 0) ::close(toServer_); // end of input: it closes its server and exits
		if (pid_ > 0) {
			// Closed input ends it; a stuck one is not worth a test's result.
			waitForEvent("\x01", std::chrono::seconds(5));
			::kill(pid_, SIGKILL);
			::waitpid(pid_, nullptr, 0);
		}
		if (fromServer_ >= 0) ::close(fromServer_);
	}

	SendspinTestServer(const SendspinTestServer&) = delete;
	SendspinTestServer& operator=(const SendspinTestServer&) = delete;

	/// Where a node dials it.
	const std::string& url() const noexcept { return url_; }

	void send(const std::string& command) {
		const std::string line = command + '\n';
		if (::write(toServer_, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
			throw std::system_error(errno, std::generic_category(), "write to the sendspin test server");
		}
	}

	/// Waits until the server has printed an event starting with `prefix` that no earlier wait returned, and returns
	/// the first such one. Nothing if its output ends or the deadline passes first.
	std::optional<std::string> waitForEvent(std::string_view prefix, std::chrono::milliseconds deadline) {
		const auto until = std::chrono::steady_clock::now() + deadline;
		for (;;) {
			for (std::size_t i = 0; i < events_.size(); ++i) {
				if (!events_[i].taken && events_[i].text.starts_with(prefix)) {
					events_[i].taken = true;
					return events_[i].text;
				}
			}
			if (!readMore(until)) return std::nullopt;
		}
	}

	/// Reads what has arrived with no wait, then how many events so far start with `prefix`.
	std::size_t count(std::string_view prefix) {
		while (readMore(std::chrono::steady_clock::now() + std::chrono::milliseconds(5))) {
		}
		std::size_t found = 0;
		for (const Event& event : events_) {
			if (event.text.starts_with(prefix)) ++found;
		}
		return found;
	}

	/// Every event so far, one per line, for a failure's report.
	std::string events() const {
		std::string text;
		for (const Event& event : events_) text += event.text + '\n';
		return text;
	}

private:
	struct Event {
		std::string text;
		bool taken = false;
	};

	bool readMore(std::chrono::steady_clock::time_point until) {
		if (ended_) return false;
		const auto left =
			std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
		if (left.count() <= 0) return false;
		pollfd ready{fromServer_, POLLIN, 0};
		const int polled = ::poll(&ready, 1, static_cast<int>(left.count()));
		if (polled < 0 && errno == EINTR) return true;
		if (polled <= 0) return false;
		char buffer[4096];
		const ssize_t got = ::read(fromServer_, buffer, sizeof buffer);
		if (got < 0 && errno == EINTR) return true;
		if (got <= 0) {
			ended_ = true;
			return false;
		}
		partial_.append(buffer, static_cast<std::size_t>(got));
		for (std::size_t end; (end = partial_.find('\n')) != std::string::npos; partial_.erase(0, end + 1)) {
			events_.push_back({partial_.substr(0, end)});
		}
		return true;
	}

	pid_t pid_ = -1;
	int toServer_ = -1;
	int fromServer_ = -1;
	bool ended_ = false;
	std::string partial_;
	std::vector<Event> events_;
	std::string url_;
};

} // namespace ear::test
