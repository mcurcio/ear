#pragma once
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace ear {
/// A lock-free ring of interleaved float frames between one writing thread and one reading thread: the one ring of
/// the handoff (realtime writer) and of the sendspin node's feed (realtime reader). Its size is a power of two, at
/// least the capacity asked for, because its positions count frames and wrap at 2^32. Nothing here locks, allocates
/// or waits, but the constructor; what a writer or a reader does with no room or nothing to read is its own.
class FrameRing {
public:
	FrameRing(std::uint32_t channels, std::uint32_t capacityFrames)
		: channels_(channels), frames_(std::bit_ceil(std::max<std::uint32_t>(capacityFrames, 1))),
		  samples_(static_cast<std::size_t>(frames_) * channels) {}
	FrameRing(const FrameRing&) = delete;
	FrameRing& operator=(const FrameRing&) = delete;

	std::uint32_t channels() const noexcept { return channels_; }
	std::uint32_t capacityFrames() const noexcept { return frames_; }

	/// Writer. Interleaves up to `frames` frames of planar `channels` (one span per channel, each >= frames). Returns
	/// frames accepted.
	std::uint32_t writePlanar(std::span<const std::span<const float>> channels, std::uint32_t frames) noexcept {
		const std::uint32_t write = written_.load(std::memory_order_relaxed);
		const std::uint32_t accepted = std::min(frames, frames_ - (write - read_.load(std::memory_order_acquire)));
		for (std::uint32_t frame = 0; frame < accepted; ++frame) {
			float* out = at(write + frame);
			for (std::uint32_t channel = 0; channel < channels_; ++channel) out[channel] = channels[channel][frame];
		}
		written_.store(write + accepted, std::memory_order_release);
		return accepted;
	}
	/// Writer. Up to `frames` frames from `interleaved` (at least frames * channels long). Returns frames accepted.
	std::uint32_t writeInterleaved(std::span<const float> interleaved, std::uint32_t frames) noexcept {
		const std::uint32_t write = written_.load(std::memory_order_relaxed);
		const std::uint32_t accepted = std::min(frames, frames_ - (write - read_.load(std::memory_order_acquire)));
		const float* in = interleaved.data();
		for (std::uint32_t frame = 0; frame < accepted; ++frame) {
			std::copy_n(in + static_cast<std::size_t>(frame) * channels_, channels_, at(write + frame));
		}
		written_.store(write + accepted, std::memory_order_release);
		return accepted;
	}
	/// Any thread. How many frames were ever written, wrapping at 2^32: where a later skipTo() ends.
	std::uint32_t writePosition() const noexcept { return written_.load(std::memory_order_acquire); }
	/// Any thread. How many frames were ever read or discarded, wrapping at 2^32: writePosition() - readPosition() is
	/// what the ring holds (exactly, on the reading thread).
	std::uint32_t readPosition() const noexcept { return read_.load(std::memory_order_acquire); }

	/// Reader. Deinterleaves up to `frames` frames into `channels` (one span per channel, each >= frames) and leaves the
	/// rest of each span untouched. Returns frames read.
	std::uint32_t readPlanar(std::span<const std::span<float>> channels, std::uint32_t frames) noexcept {
		const std::uint32_t read = read_.load(std::memory_order_relaxed);
		const std::uint32_t got = std::min(frames, written_.load(std::memory_order_acquire) - read);
		for (std::uint32_t frame = 0; frame < got; ++frame) {
			const float* in = at(read + frame);
			for (std::uint32_t channel = 0; channel < channels_; ++channel) channels[channel][frame] = in[channel];
		}
		read_.store(read + got, std::memory_order_release);
		return got;
	}
	/// Reader. The oldest frames, interleaved, as far as the ring's end; the rest (wrapped to its start) after skip().
	/// They stay in the ring until skip() takes them.
	std::span<const float> readSpan() const noexcept {
		const std::uint32_t read = read_.load(std::memory_order_relaxed);
		const std::uint32_t available = written_.load(std::memory_order_acquire) - read;
		const std::uint32_t run = std::min(available, frames_ - (read & (frames_ - 1)));
		return {at(read), static_cast<std::size_t>(run) * channels_};
	}
	/// Reader. Discards up to `frames` of the oldest frames. Returns how many.
	std::uint32_t skip(std::uint32_t frames) noexcept {
		const std::uint32_t read = read_.load(std::memory_order_relaxed);
		const std::uint32_t got = std::min(frames, written_.load(std::memory_order_acquire) - read);
		read_.store(read + got, std::memory_order_release);
		return got;
	}
	/// Reader. Discards every frame before `position`, a writePosition() taken earlier; one it has read past already
	/// discards nothing.
	void skipTo(std::uint32_t position) noexcept {
		const std::uint32_t read = read_.load(std::memory_order_relaxed);
		if (static_cast<std::int32_t>(position - read) > 0) read_.store(position, std::memory_order_release);
	}

private:
	float* at(std::uint32_t position) noexcept {
		return samples_.data() + static_cast<std::size_t>(position & (frames_ - 1)) * channels_;
	}
	const float* at(std::uint32_t position) const noexcept {
		return samples_.data() + static_cast<std::size_t>(position & (frames_ - 1)) * channels_;
	}

	std::uint32_t channels_;
	std::uint32_t frames_;
	std::vector<float> samples_; // interleaved; frames_ frames
	// Apart, so the two threads do not share a cache line.
	alignas(64) std::atomic<std::uint32_t> written_{0};
	alignas(64) std::atomic<std::uint32_t> read_{0};
};
} // namespace ear
