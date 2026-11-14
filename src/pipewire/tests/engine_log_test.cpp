// ear's own pw_log (docs/output-records.md): PipeWire's messages from the main thread and from the
// data loop become unattributed log entries with their level and topic; the context hands ear's log to every plugin;
// a full ring drops and the drain counts what it dropped. Every wait is for the bell; its deadline only fails a test.
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <poll.h>

#include <pipewire/pipewire.h>
#include <spa/support/plugin.h>

#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/engine_log.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;

std::unique_ptr<Engine> makeEngine() {
	for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS",
	                         "DBUS_SYSTEM_BUS_ADDRESS"}) {
		unsetenv(name);
	}
	auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, ear::SampleRate::fixed<48000>(), 256});
	if (!engine) FAIL("Engine::create: " << engine.error().detail);
	return std::move(*engine);
}

struct Entries final : LogSink {
	std::vector<LogEntry> got;
	void log(const LogEntry& entry) override { got.push_back(entry); }
	bool has(const std::string& text) const {
		for (const LogEntry& e : got)
			if (e.text == text) return true;
		return false;
	}
	const LogEntry* find(const std::string& text) const {
		for (const LogEntry& e : got)
			if (e.text == text) return &e;
		return nullptr;
	}
};

bool bellRings(const EngineLog& log) {
	pollfd ready{log.bell(), POLLIN, 0};
	return ::poll(&ready, 1, 5000) == 1;
}

/// Drains until `text` arrived or the bell stops ringing.
void drainUntil(EngineLog& log, Entries& entries, const std::string& text) {
	while (!entries.has(text) && bellRings(log)) log.drain(entries);
}

int logFromDataLoop(spa_loop* /*loop*/, bool /*async*/, std::uint32_t /*seq*/, const void* /*data*/,
                    std::size_t /*size*/, void* /*user*/) {
	pw_log_error("from the data loop %d", 2);
	return 0;
}

} // namespace

TEST_CASE("PipeWire's messages from the main thread and the data loop become log entries", "[engine_log]") {
	EngineLog& log = EngineLog::install();
	const auto engine = makeEngine();
	pw_log_warn("from the main thread %d", 1);
	Entries entries;
	drainUntil(log, entries, "from the main thread 1");
	pw_data_loop_invoke(pw_context_get_data_loop(engine->context()), &logFromDataLoop, 0, nullptr, 0, true, nullptr);
	drainUntil(log, entries, "from the data loop 2");

	const LogEntry* main = entries.find("from the main thread 1");
	REQUIRE(main != nullptr);
	CHECK(main->level == LogLevel::warn);
	CHECK(main->source == "pipewire");
	CHECK_FALSE(main->node);
	const LogEntry* data = entries.find("from the data loop 2");
	REQUIRE(data != nullptr);
	CHECK(data->level == LogLevel::error);
}

TEST_CASE("The context gives ear's log to the plugins it loads", "[engine_log]") {
	EngineLog::install();
	const auto engine = makeEngine();
	std::uint32_t count = 0;
	const spa_support* support = pw_context_get_support(engine->context(), &count);
	CHECK(spa_support_find(support, count, SPA_TYPE_INTERFACE_Log) == pw_log_get());
	pw_log_warn("probe");
	CHECK(pw_log_get()->level >= SPA_LOG_LEVEL_WARN);
}

TEST_CASE("A full ring drops entries and the drain counts them", "[engine_log]") {
	EngineLog& log = EngineLog::install();
	Entries before;
	log.drain(before); // whatever earlier tests left
	for (int i = 0; i < 300; ++i) pw_log_warn("flood %d", i);
	REQUIRE(bellRings(log));
	Entries entries;
	log.drain(entries);
	REQUIRE(entries.got.size() == 257);
	CHECK(entries.got.front().text == "flood 0");
	CHECK(entries.got[255].text == "flood 255");
	CHECK(entries.got.back().source == "ear");
	CHECK(entries.got.back().text == "44 PipeWire log entries dropped: the ring was full");
	Entries after;
	pw_log_warn("after the flood");
	drainUntil(log, after, "after the flood");
	CHECK(after.has("after the flood"));
}

TEST_CASE("alsa-lib's errors become log entries from alsa-lib", "[engine_log]") {
	EngineLog& log = EngineLog::install();
	// alsa-lib calls the handler through its variable snd_lib_error, as its SNDERR does.
	using Handler = void (*)(const char*, int, const char*, int, const char*, ...);
	void* library = ::dlopen("libasound.so.2", RTLD_NOW | RTLD_NOLOAD);
	REQUIRE(library != nullptr);
	auto* handler = static_cast<Handler*>(::dlsym(library, "snd_lib_error"));
	REQUIRE(handler != nullptr);
	(*handler)("confmisc.c", 165, "snd_config_get_card", 0, "Cannot get card index for %s", "Nope");
	(*handler)("pcm.c", 7, "snd_pcm_open", -2, "open failed");
	Entries entries;
	drainUntil(log, entries, "pcm.c:7:(snd_pcm_open) open failed: No such file or directory");
	const LogEntry* card = entries.find("confmisc.c:165:(snd_config_get_card) Cannot get card index for Nope");
	REQUIRE(card != nullptr);
	CHECK(card->source == "alsa-lib");
	CHECK(card->level == LogLevel::error);
	CHECK(card->topic.empty());
	CHECK(entries.has("pcm.c:7:(snd_pcm_open) open failed: No such file or directory"));
	::dlclose(library);
}
