// The recording kind's adapter over a real confined folder, as main.cpp wires it, for tests that run "file" through
// the harness (ear/testing/node_harness.hpp). Header-only; the test links ear_folder and ear_core.
#pragma once

#include <expected>
#include <memory>
#include <string>
#include <utility>

#include <unistd.h>

#include "ear/core/node_host.hpp"
#include "ear/folder/confined_folder.hpp"

namespace ear::test {

class ConfinedRecordings final : public RecordingAdapter {
public:
	explicit ConfinedRecordings(std::shared_ptr<const folder::ConfinedFolder> folder) : folder_(std::move(folder)) {}
	std::expected<std::unique_ptr<RecordingFile>, std::string> open(const NodeName&, const std::string& path) override {
		auto made = folder_->create(path);
		if (!made) return std::unexpected(std::move(made.error().detail));
		return std::make_unique<Recording>(*made);
	}

private:
	class Recording final : public RecordingFile {
	public:
		explicit Recording(int fd) noexcept : fd_(fd) {}
		Recording(const Recording&) = delete;
		Recording& operator=(const Recording&) = delete;
		~Recording() override { ::close(fd_); }
		int descriptor() const noexcept override { return fd_; }

	private:
		int fd_;
	};
	std::shared_ptr<const folder::ConfinedFolder> folder_;
};

} // namespace ear::test
