// What a program tells, recorded for a test of Programs (ear_runtime_tests, ear_runtime_reaper_tests): every event
// under one lock, and a wait for the one a test needs. The wait is event-gated: each event wakes it, and its deadline
// only fails the test.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ear/core/program.hpp"

namespace ear::test {

class RecordedProgram final : public ProgramEvents {
public:
	void started() override {
		tell([&] { isStarted = true; });
	}
	void ready() override {
		tell([&] { isReady = true; });
	}
	void refused(std::string why) override {
		tell([&] { refusal = std::move(why); });
	}
	void fed(std::string bytes) override {
		tell([&] { feed += bytes; });
	}
	void said(LogEntry entry) override {
		tell([&] { lines.push_back(std::move(entry)); });
	}
	void exited(ProgramExit how) override {
		if (atExit) atExit(); // on the adapter's thread, before the end is recorded
		tell([&] { exit = std::move(how); });
	}

	/// Waits until `done` holds of what was told (read under the lock); false past the deadline.
	bool until(const std::function<bool(const RecordedProgram&)>& done,
	           std::chrono::seconds deadline = std::chrono::seconds(10)) {
		std::unique_lock lock(mutex_);
		return changed_.wait_for(lock, deadline, [&] { return done(*this); });
	}
	/// Waits until it was told exited or refused, the last thing it tells.
	bool ended(std::chrono::seconds deadline = std::chrono::seconds(10)) {
		return until([](const RecordedProgram& program) { return program.exit || program.refusal; }, deadline);
	}
	/// The lines of `topic` ("stdout", "stderr") told so far, by text. Under the lock: call it inside until, or after
	/// ended.
	std::vector<std::string> texts(std::string_view topic) const {
		std::vector<std::string> found;
		for (const LogEntry& line : lines)
			if (line.topic == topic) found.push_back(line.text);
		return found;
	}

	/// Called inside exited, before it is recorded: what a test checks at that moment.
	std::function<void()> atExit;

	// Read inside until's predicate, or once ended() returned true.
	bool isStarted = false;
	bool isReady = false;
	std::optional<std::string> refusal;
	std::string feed;
	std::vector<LogEntry> lines;
	std::optional<ProgramExit> exit;

private:
	template <class Change> void tell(Change change) {
		{
			const std::lock_guard lock(mutex_);
			change();
		}
		changed_.notify_all();
	}
	std::mutex mutex_;
	std::condition_variable changed_;
};

} // namespace ear::test
