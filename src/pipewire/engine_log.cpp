// ear's own pw_log (engine_log.hpp). The ring is a bounded multi-producer queue of fixed slots (Vyukov's): a producer
// claims a slot by one compare-and-swap on the write position and publishes it by its sequence number; PipeWire may
// log from any of its threads at once. Nothing on the producer's side allocates, locks or blocks; vsnprintf formats
// into the slot itself.
#include "ear/pipewire/engine_log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include <dlfcn.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <pipewire/log.h>
#include <spa/support/log.h>

namespace ear::pipewire {

namespace {

constexpr std::size_t slotCount = 256; // a power of two
constexpr std::size_t textBound = 1024;
constexpr std::size_t topicBound = 64;

struct Slot {
	std::atomic<std::size_t> sequence{0};
	spa_log_level level = SPA_LOG_LEVEL_NONE;
	const char* origin = "pipewire"; // a string literal: "pipewire" or "alsa-lib"
	bool cut = false;
	char topic[topicBound] = {};
	char text[textBound] = {};
};

struct Ring {
	std::array<Slot, slotCount> slots;
	alignas(64) std::atomic<std::size_t> writeAt{0};
	alignas(64) std::size_t readAt = 0; // the drain's, under drainMutex
	std::atomic<std::uint64_t> dropped{0};
	std::atomic<bool> rung{false};
	int bell = -1;
	std::mutex drainMutex;

	Ring() {
		for (std::size_t i = 0; i < slotCount; ++i) slots[i].sequence.store(i, std::memory_order_relaxed);
		bell = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	}

	/// Any thread, realtime included. `fill` formats the text into the slot's buffer (vsnprintf's contract: the length
	/// it wanted, or below 0).
	template <typename Fill>
	void push(const char* origin, spa_log_level level, const char* topic, Fill&& fill) noexcept {
		std::size_t at = writeAt.load(std::memory_order_relaxed);
		Slot* slot = nullptr;
		while (true) {
			slot = &slots[at & (slotCount - 1)];
			const std::size_t sequence = slot->sequence.load(std::memory_order_acquire);
			const auto difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(at);
			if (difference == 0) {
				if (writeAt.compare_exchange_weak(at, at + 1, std::memory_order_relaxed)) break;
			} else if (difference < 0) {
				dropped.fetch_add(1, std::memory_order_relaxed);
				ring();
				return;
			} else {
				at = writeAt.load(std::memory_order_relaxed);
			}
		}
		slot->origin = origin;
		slot->level = level;
		const int wrote = fill(slot->text, textBound);
		slot->cut = wrote >= static_cast<int>(textBound);
		if (wrote < 0) slot->text[0] = '\0';
		std::size_t length = 0;
		while (length + 1 < topicBound && topic[length] != '\0') ++length;
		std::memcpy(slot->topic, topic, length);
		slot->topic[length] = '\0';
		slot->sequence.store(at + 1, std::memory_order_release);
		ring();
	}

	/// The gated bell: one eventfd write until the next drain.
	void ring() noexcept {
		if (rung.exchange(true, std::memory_order_acq_rel)) return;
		const std::uint64_t one = 1;
		[[maybe_unused]] const ssize_t ignored = ::write(bell, &one, sizeof one);
	}
};

Ring& ring() {
	static Ring instance;
	return instance;
}

LogLevel levelOf(spa_log_level level) {
	switch (level) {
	case SPA_LOG_LEVEL_ERROR:
		return LogLevel::error;
	case SPA_LOG_LEVEL_WARN:
		return LogLevel::warn;
	case SPA_LOG_LEVEL_INFO:
		return LogLevel::info;
	default:
		return LogLevel::debug;
	}
}

void logtv(void* /*object*/, spa_log_level level, const spa_log_topic* topic, const char* /*file*/, int /*line*/,
           const char* /*func*/, const char* format, va_list args) {
	const char* name = topic != nullptr && topic->topic != nullptr ? topic->topic : "";
	ring().push("pipewire", level, name,
	            [&](char* text, std::size_t size) { return std::vsnprintf(text, size, format, args); });
}

/// alsa-lib's error handler (snd_lib_error_handler_t), as alsa-lib's own default words it: "<file>:<line>:(<function>)
/// <message>[: <error>]". alsa-lib calls it on whatever thread failed, the data loop included.
void alsaError(const char* file, int line, const char* function, int error, const char* format, ...) {
	va_list args;
	va_start(args, format);
	ring().push("alsa-lib", SPA_LOG_LEVEL_ERROR, "", [&](char* text, std::size_t size) {
		const int head = std::snprintf(text, size, "%s:%d:(%s) ", file != nullptr ? file : "?", line,
		                               function != nullptr ? function : "?");
		if (head < 0 || static_cast<std::size_t>(head) >= size) return head;
		const int body = std::vsnprintf(text + head, size - static_cast<std::size_t>(head), format, args);
		if (body < 0) return body;
		int total = head + body;
		const char* described = error != 0 ? strerrordesc_np(error < 0 ? -error : error) : nullptr;
		if (described != nullptr && static_cast<std::size_t>(total) < size)
			total += std::snprintf(text + total, size - static_cast<std::size_t>(total), ": %s", described);
		return total;
	});
	va_end(args);
}

/// Points alsa-lib's process-wide error handler at alsaError. The image carries alsa-lib's run-time library only
/// (the ALSA plugin links it), so it is reached by name, not linked; where it is missing there is no ALSA to log.
void installAlsaHandler() {
	using Handler = void (*)(const char*, int, const char*, int, const char*, ...);
	using SetHandler = int (*)(Handler);
	static void* const library = ::dlopen("libasound.so.2", RTLD_NOW | RTLD_GLOBAL);
	if (library == nullptr) return;
	if (auto* set = reinterpret_cast<SetHandler>(::dlsym(library, "snd_lib_error_set_handler"))) set(&alsaError);
}

void logt(void* object, spa_log_level level, const spa_log_topic* topic, const char* file, int line, const char* func,
          const char* format, ...) {
	va_list args;
	va_start(args, format);
	logtv(object, level, topic, file, line, func, format, args);
	va_end(args);
}

void logv(void* object, spa_log_level level, const char* file, int line, const char* func, const char* format,
          va_list args) {
	logtv(object, level, nullptr, file, line, func, format, args);
}

void logPlain(void* object, spa_log_level level, const char* file, int line, const char* func, const char* format,
              ...) {
	va_list args;
	va_start(args, format);
	logtv(object, level, nullptr, file, line, func, format, args);
	va_end(args);
}

/// Topics follow the log's one level (has_custom_level stays false).
void topicInit(void* /*object*/, spa_log_topic* /*topic*/) {}

const spa_log_methods methods{
	.version = SPA_VERSION_LOG_METHODS,
	.log = logPlain,
	.logv = logv,
	.logt = logt,
	.logtv = logtv,
	.topic_init = topicInit,
};

spa_log& spaLog() {
	static spa_log instance{
		.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, SPA_VERSION_LOG, &methods, nullptr),
		.level = SPA_LOG_LEVEL_WARN,
	};
	return instance;
}

} // namespace

EngineLog::EngineLog() { ring(); }

EngineLog& EngineLog::install() {
	static EngineLog instance;
	pw_log_set(&spaLog());
	installAlsaHandler();
	return instance;
}

int EngineLog::bell() const noexcept { return ring().bell; }

void EngineLog::drain(LogSink& sink) {
	Ring& r = ring();
	const std::scoped_lock lock(r.drainMutex);
	std::uint64_t count = 0;
	[[maybe_unused]] const ssize_t ignored = ::read(r.bell, &count, sizeof count);
	// Before reading the slots: a producer that publishes after this rings again. An exchange, not a store: a store
	// then a load has no StoreLoad order, so this drain could miss a slot just published while its producer still read
	// true and skipped the bell (a lost wakeup). As read-modify-writes on one atomic, the producer's exchange comes
	// either after this one (it reads false and rings) or before it (this one reads its release and sees the slot).
	r.rung.exchange(false, std::memory_order_acq_rel);
	while (true) {
		Slot& slot = r.slots[r.readAt & (slotCount - 1)];
		if (slot.sequence.load(std::memory_order_acquire) != r.readAt + 1) break;
		LogEntry entry{.level = levelOf(slot.level), .source = slot.origin, .text = slot.text, .topic = slot.topic};
		entry.cut = slot.cut;
		slot.sequence.store(r.readAt + slotCount, std::memory_order_release);
		++r.readAt;
		// Many PipeWire messages end in a newline of their own.
		while (!entry.text.empty() && entry.text.back() == '\n') entry.text.pop_back();
		sink.log(entry);
	}
	if (const std::uint64_t dropped = r.dropped.exchange(0, std::memory_order_relaxed); dropped > 0)
		sink.log({.level = LogLevel::warn,
		          .source = "ear",
		          .text = std::format("{} PipeWire log entries dropped: the ring was full", dropped)});
}

} // namespace ear::pipewire
