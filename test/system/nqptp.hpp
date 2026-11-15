// nqptp, AirPlay 2's timing service, run by an airplay system test as the operator runs it beside ear
// (docs/node-types.md#airplay; docs/testing.md#nqptp): the dev image's EAR_NQPTP, up once it answers on its control port.
// Every wait is for an event (a file nqptp makes, its exit); a deadline only fails the test.
#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "child_process.hpp"

namespace ear::test {

/// Whether the POSIX shared-memory object `name` ("/<name>", shm_open(3)'s) exists: glibc keeps them in /dev/shm.
inline bool sharedMemoryExists(std::string_view name) {
	return std::filesystem::exists(std::filesystem::path("/dev/shm") / name.substr(name.starts_with('/') ? 1 : 0));
}

class Nqptp {
public:
	/// Starts nqptp and waits until it is up: its own channel "/ear-test-up-<its pid>" appears in /dev/shm once nqptp
	/// read the control message asking for it ("<name> T" on localhost UDP 9000). The message is sent again only when
	/// the kernel answers ECONNREFUSED, its word that nothing is bound to 9000 yet.
	explicit Nqptp(std::chrono::milliseconds patience) : process_(EAR_NQPTP, {}) {
		const std::string channel = "/ear-test-up-" + std::to_string(process_.pid());
		const int watch = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
		REQUIRE(watch >= 0);
		REQUIRE(::inotify_add_watch(watch, "/dev/shm", IN_CREATE) >= 0);
		const int control = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		REQUIRE(control >= 0);
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_port = htons(9000);
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(::connect(control, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0);
		const std::string message = channel + " T";
		const auto send = [&] { return ::send(control, message.data(), message.size(), 0); };
		bool up = false;
		const auto until = std::chrono::steady_clock::now() + patience;
		if (send() < 0 && errno != ECONNREFUSED) FAIL("nqptp's control message: " << std::strerror(errno));
		while (!up) {
			up = sharedMemoryExists(channel);
			if (up) break;
			const auto left =
				std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
			if (left <= 0) break;
			std::array<pollfd, 2> ready{pollfd{watch, POLLIN, 0}, pollfd{control, POLLIN, 0}};
			if (::poll(ready.data(), ready.size(), static_cast<int>(left)) <= 0) continue;
			if ((ready[0].revents & POLLIN) != 0) {
				std::array<char, 4096> events{};
				while (::read(watch, events.data(), events.size()) > 0) {
				}
			}
			if ((ready[1].revents & (POLLERR | POLLIN)) != 0) {
				// The kernel's ICMP answer, held as the socket's error: nothing is bound yet, so ask again.
				std::array<char, 64> ignored{};
				if (::recv(control, ignored.data(), ignored.size(), 0) < 0 && errno == ECONNREFUSED) {
					if (send() < 0 && errno != ECONNREFUSED) FAIL("nqptp's control message: " << std::strerror(errno));
				}
			}
		}
		::close(control);
		::close(watch);
		INFO("nqptp printed:\n" << process_.output());
		REQUIRE(up);
	}

	/// SIGTERM, which nqptp answers by removing every channel it made, then its exit status (-1: none in time, or
	/// killed by a signal).
	int stop(std::chrono::milliseconds patience) {
		if (stopped_) return status_;
		stopped_ = true;
		process_.signal(SIGTERM);
		status_ = process_.waitForExit(patience).value_or(-1);
		return status_;
	}

	/// Stopped as stop() does, never killed: a killed nqptp leaves its channels in /dev/shm for the next test to find.
	~Nqptp() { stop(std::chrono::seconds(10)); }

	Nqptp(const Nqptp&) = delete;
	Nqptp& operator=(const Nqptp&) = delete;

private:
	ChildProcess process_;
	bool stopped_ = false;
	int status_ = -1;
};

} // namespace ear::test
