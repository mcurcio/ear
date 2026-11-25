// Changes in place through NodeHost (docs/architecture.md#changes-in-place), by a test type run in the type harness:
// the change version called on a rested node, its holds acknowledged before the commit point and faded in after, its
// staged posts released at the commit in order (and destroyed unrun when the change does not commit), a new code
// swapped at one position, supersession before and after the commit point, a stop, a removal and a disable during a
// change, and the fallback to a restart with each of its reasons. The aggregate's own rules are node_lifecycle_test's.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace reconfigure_test {
using namespace ear;

constexpr auto gainRow = setting::optional("gain", setting::number(0, 100)).inPlace();
constexpr auto levelRow = setting::optional("level", setting::number(0, 100)).inPlace();
constexpr auto sizeRow = setting::defaulted("size", setting::number(1, 4), 1); // restart-only
constexpr auto in = bank::input<"in">;
constexpr auto out = bank::output<"out">;
constexpr auto side = bank::output<"side">;

/// The loop's state: what the work posted to it saw, in order; it lives across a change.
struct Work {
	std::vector<std::string> seen;
};
constexpr auto work = loop::strand<"work", Work>;

constexpr TypeSpec spec{.settings = SettingsTable<gainRow, levelRow, sizeRow>{"reconf"},
                        .banks = Banks<in, out, side>{},
                        .loops = Loops<work>{}};
constexpr TypeSpec slowSpec{.settings = SettingsTable<gainRow, levelRow, sizeRow>{"slow"},
                            .banks = Banks<in, out, side>{},
                            .loops = Loops<work>{},
                            .lifecycle = Lifecycle{.reconfigureDeadline = std::chrono::seconds(30)}};
constexpr TypeSpec bareSpec{.settings = SettingsTable<gainRow, levelRow, sizeRow>{"bare"},
                            .banks = Banks<in, out, side>{},
                            .loops = Loops<work>{}};

/// What the change version does, set by each test.
struct Script {
	enum class Answer { applied, appliedCode, restart, throws, breakRule } answer = Answer::applied;
	bool holdOut = true;
	bool post = true;
	bool startThrows = false;
	float codeLevel = 0.25F;
};
Script script;
/// What the type was called with, in order: "configure g<gain>", "change g<old>->g<now>".
std::vector<std::string> calls;
/// How many times stop() was called.
int stops = 0;
/// Every Work the loop saw, by what it last held (read once a test ran the loops).
std::vector<std::string> worked;

std::string gainOf(const auto& settings) {
	const auto gain = settings.template get<gainRow>();
	return "g" + (gain ? std::to_string(*gain) : std::string("-"));
}

/// Writes `level` on every output.
template <const auto& Spec> struct Level {
	float level;
	void process(Io<Spec> io, const GraphCycle&) noexcept {
		for (auto member : io[out]) std::ranges::fill(member, level);
		for (auto member : io[side]) std::ranges::fill(member, level);
	}
};

template <const auto& Spec> struct Reconf {
	using Checked = CheckedOf<Spec>;
	static Members<Spec> ports(const Checked&) { return {in(members(1)), out(members(1)), side(members(1))}; }
	Configured configure(CreateOf<Spec>& cx, const Checked& s) {
		calls.push_back("configure " + gainOf(s));
		return cx.applied(Level<Spec>{1.0F}, cx.state(work));
	}
	void start() {
		if (script.startThrows) throw std::runtime_error("no device");
	}
	void stop() { ++stops; }
	Changed configure(ChangeOf<Spec>& cx, const Checked& old, const Checked& now) {
		calls.push_back("change " + gainOf(old) + "->" + gainOf(now));
		if (script.holdOut) cx.hold(out);
		if (script.post)
			cx.post(work, [what = "post " + gainOf(now)](Work& w) {
				w.seen.push_back(what);
				worked.push_back(what);
			});
		switch (script.answer) {
		case Script::Answer::applied:
			return cx.applied();
		case Script::Answer::appliedCode:
			return cx.applied(Level<Spec>{script.codeLevel});
		case Script::Answer::restart:
			return Changed::restart("cannot take it");
		case Script::Answer::throws:
			throw std::runtime_error("blew up");
		case Script::Answer::breakRule:
			cx.declareLatency(in, out, Latency{});
			cx.declareLatency(in, out, Latency{}); // one path declared twice: a broken rule
			return cx.applied();
		}
		return cx.applied();
	}
};

/// The same type without a change version.
struct Bare {
	using Checked = CheckedOf<bareSpec>;
	static Members<bareSpec> ports(const Checked&) { return {in(members(1)), out(members(1)), side(members(1))}; }
	Configured configure(CreateOf<bareSpec>& cx, const Checked& s) {
		calls.push_back("configure " + gainOf(s));
		return cx.applied(Level<bareSpec>{1.0F}, cx.state(work));
	}
};

using Harness = testing::TypeHarness<spec, Reconf<spec>>;

void reset(Script asked = {}) {
	script = asked;
	calls.clear();
	worked.clear();
	stops = 0;
}

/// Added and running with `settings`; with autoComplete off its drive completed by hand.
template <class H = Harness>
std::unique_ptr<H> running(testing::HarnessOptions options = {}, NodeSettings settings = NodeSettings::object()) {
	auto h = std::make_unique<H>(std::move(options));
	REQUIRE_FALSE(h->parse(settings));
	h->add();
	h->blockReady();
	if (h->phase() != "running") h->node().completeBarrier();
	REQUIRE(h->phase() == "running");
	h->runLoops();
	return h;
}

testing::HarnessOptions manual() { return testing::HarnessOptions{.autoComplete = false}; }

bool toldPhase(const testing::NodeHarness& h, NodePhase phase) {
	return std::ranges::any_of(h.healths(), [&](const NodeHealth& one) { return one.phase == phase; });
}

/// The fallback's restart, as its stopping was told: its reason, if any was told.
std::optional<NodeCause::FellBack> fellBackReason(const testing::NodeHarness& h) {
	for (const NodeHealth& one : h.healths())
		if (one.phase == NodePhase::stopping && one.cause && one.cause->restart == NodeCause::Kind::fellBack)
			return one.cause->reason;
	return std::nullopt;
}

bool said(const testing::NodeHarness& h, std::string_view code) {
	return std::ranges::any_of(h.diagnostics(), [&](const auto& one) { return one.code == code; });
}

} // namespace reconfigure_test
using namespace reconfigure_test;

TEST_CASE("In place ends running with the same configuration: no restart, no drive, the loop's state kept",
          "[reconfigure]") {
	reset();
	auto owned = running();
	auto& h = *owned;
	h->runLoops();
	CHECK(h.change({{"gain", 10}}) == NodeOutcome::reconfigure);
	CHECK(h->phase() == "running");
	CHECK(toldPhase(h.node(), NodePhase::reconfiguring));
	CHECK_FALSE(toldPhase(h.node(), NodePhase::stopping));
	CHECK(calls == std::vector<std::string>{"configure g-", "change g-->g10"});
	CHECK(h->health().cause == std::nullopt);
	CHECK(h->swaps() == 0);
	// The staged post runs on the same loop state, once released at the commit.
	h->runLoops();
	CHECK(worked == std::vector<std::string>{"post g10"});
	// A second change sees the first's settings as the applied ones.
	CHECK(h.change({{"gain", 20}}) == NodeOutcome::reconfigure);
	CHECK(calls.back() == "change g10->g20");
	// The reconfiguring the change told carried its keys.
	const auto changing =
		std::ranges::find_if(h->healths(), [](const NodeHealth& one) { return one.phase == NodePhase::reconfiguring; });
	REQUIRE(changing != h->healths().end());
	CHECK(changing->keys == std::vector<std::string>{"gain"});
}

TEST_CASE("In place from stopped ends stopped, and does not restart on the next converge", "[reconfigure]") {
	reset();
	auto owned = running();
	auto& h = *owned;
	h.stop();
	REQUIRE(h->phase() == "stopped");
	CHECK(h.change({{"level", 3}}) == NodeOutcome::reconfigure);
	CHECK(h->phase() == "stopped");
	CHECK(calls == std::vector<std::string>{"configure g-", "change g-->g-"});
	h.start();
	CHECK(h->phase() == "running");
	CHECK(calls.size() == 2); // the start drove the configuration it had
}

TEST_CASE("A type with no change version restarts (cause settings), with no reconfiguring", "[reconfigure]") {
	reset();
	auto owned = running<testing::TypeHarness<bareSpec, Bare>>();
	auto& h = *owned;
	CHECK(h.change({{"gain", 10}}) == NodeOutcome::reconfigure); // the plan's outcome stays what it was
	CHECK(h->phase() == "running");
	CHECK(calls == std::vector<std::string>{"configure g-", "configure g10"});
	CHECK_FALSE(toldPhase(h.node(), NodePhase::reconfiguring));
	CHECK(toldPhase(h.node(), NodePhase::stopping));
}

TEST_CASE("A node not yet rested takes a change in place as a restart", "[reconfigure]") {
	reset();
	Harness h;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add(); // configured, waiting for its block: not rested
	CHECK(h.change({{"gain", 10}}) == NodeOutcome::reconfigure);
	h.blockReady();
	CHECK(h.phase() == "running");
	CHECK(calls == std::vector<std::string>{"configure g-", "configure g10"});
	CHECK_FALSE(toldPhase(h.node(), NodePhase::reconfiguring));
}

TEST_CASE("A change that cannot finish falls back to a restart with the newest settings, cause fell-back",
          "[reconfigure][fallback]") {
	SECTION("restart-asked: the change version answered restart") {
		reset({.answer = Script::Answer::restart});
		auto owned = running();
		auto& h = *owned;
		h.change({{"gain", 10}});
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::restartAsked);
		CHECK(said(h.node(), "ear.reconfigure-fell-back"));
		CHECK(calls == std::vector<std::string>{"configure g-", "change g-->g10", "configure g10"});
		CHECK(h->phase() == "running");
		h->runLoops();
		CHECK(worked.empty()); // its staged post was destroyed unrun
	}
	SECTION("failed: the change version threw") {
		reset({.answer = Script::Answer::throws});
		auto owned = running();
		auto& h = *owned;
		h.change({{"gain", 10}});
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::failed);
		CHECK(calls.back() == "configure g10");
		CHECK(h->phase() == "running");
	}
	SECTION("restart-requested: node.restart during a change") {
		reset();
		auto owned = running(manual());
		auto& h = *owned;
		h.change({{"gain", 10}});
		REQUIRE(h->phase() == "reconfiguring"); // its hold waits for its acknowledgement
		CHECK(h.restart() == LifecycleReply{LifecycleApplied::restart});
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::restartRequested);
	}
}

TEST_CASE("A rule broken in a change fails the node lost", "[reconfigure]") {
	reset({.answer = Script::Answer::breakRule});
	auto owned = running();
	auto& h = *owned;
	h.change({{"gain", 10}});
	REQUIRE(h->phase() == "failed");
	CHECK(h->health().reason->code == FailureReason::Code::lost);
	CHECK_FALSE(fellBackReason(h.node()));
}

TEST_CASE("Holds are acknowledged before the commit point and faded back in after it", "[reconfigure][hold]") {
	reset();
	auto owned = running(manual());
	auto& h = *owned;
	for (int i = 0; i < 4; ++i) h.process(); // the drive's fade-in long over
	h.change({{"gain", 10}});
	REQUIRE(h->phase() == "reconfiguring");
	CHECK(h.held(out));
	CHECK_FALSE(h.held(side));
	h.process(); // 256 frames: the hold's 240 reach zero, and the cycle acknowledges it
	CHECK(h.lastHoldGain(out) == 0.0F);
	CHECK(h.output(out, 0)[255] == 0.0F);
	CHECK(h.output(side, 0)[255] == 1.0F);
	// The commit point came with the acknowledgement: committed at once (no code to swap), its hold released.
	CHECK(h->phase() == "running");
	CHECK_FALSE(h.held(out));
	h.process();
	CHECK(h.output(out, 0)[0] == 0.0F);
	CHECK(h.output(out, 0)[255] == 1.0F); // faded back in
}

TEST_CASE("A hold never acknowledged goes on after 100 ms, said", "[reconfigure][hold]") {
	reset();
	auto owned = running(manual());
	auto& h = *owned;
	h.change({{"gain", 10}});
	REQUIRE(h->phase() == "reconfiguring");
	h->advance(std::chrono::milliseconds(100));
	CHECK(said(h.node(), "ear.fade-unacknowledged"));
	CHECK(h->phase() == "running");
}

TEST_CASE("A new code is swapped in at the commit point, at one position with no fade", "[reconfigure][swap]") {
	reset({.answer = Script::Answer::appliedCode, .holdOut = false});
	auto owned = running();
	auto& h = *owned;
	for (int i = 0; i < 4; ++i) h.process();
	h.change({{"gain", 10}});
	CHECK(h->phase() == "running");
	CHECK(h->swaps() == 1);
	h.process();
	CHECK(h.output(out, 0)[0] == 0.25F);
	CHECK(h.lastFadeGain(out) == 1.0F);
	CHECK(calls.size() == 2);
}

TEST_CASE("Supersession: a newer change before the commit point calls the change version again, from the applied "
          "settings",
          "[reconfigure][supersede]") {
	reset();
	auto owned = running(manual());
	auto& h = *owned;
	h.change({{"gain", 10}});
	REQUIRE(h->phase() == "reconfiguring");
	h.change({{"gain", 20}});
	CHECK(said(h.node(), "ear.reconfigure-superseded"));
	CHECK(calls == std::vector<std::string>{"configure g-", "change g-->g10", "change g-->g20"});
	h->acknowledgeHolds();
	CHECK(h->phase() == "running");
	h->runLoops();
	CHECK(worked == std::vector<std::string>{"post g20"}); // the superseded change's post never ran
}

TEST_CASE("A newer change after the commit point waits for it, as next, then changes again",
          "[reconfigure][supersede]") {
	reset({.answer = Script::Answer::appliedCode, .holdOut = false});
	auto owned = running(manual());
	auto& h = *owned;
	h.change({{"gain", 10}});
	REQUIRE(h->phase() == "reconfiguring"); // the swap's invoke in flight: committing
	h.change({{"gain", 20}, {"level", 1}});
	CHECK(h->health().next == std::vector<std::string>{"gain", "level"});
	CHECK(calls.size() == 2); // it waits
	h->completeBarrier();     // the commit completed: the newer change begins
	CHECK(calls.back() == "change g10->g20");
	h->completeBarrier();
	CHECK(h->phase() == "running");
}

TEST_CASE("A newer restart-only change before the commit point restarts, cause settings", "[reconfigure][supersede]") {
	reset();
	auto owned = running(manual());
	auto& h = *owned;
	h.change({{"gain", 10}});
	REQUIRE(h->phase() == "reconfiguring");
	CHECK(h.change({{"gain", 10}, {"size", 2}}) == NodeOutcome::restart);
	const auto restarted = std::ranges::find_if(h->healths(), [](const NodeHealth& one) {
		return one.phase == NodePhase::stopping && one.cause && one.cause->restart == NodeCause::Kind::settings;
	});
	CHECK(restarted != h->healths().end());
}

TEST_CASE("A stop, a removal or a disable during a change ends it before its commit point", "[reconfigure]") {
	reset();
	auto owned = running(manual());
	auto& h = *owned;
	h.change({{"gain", 10}});
	REQUIRE(h->phase() == "reconfiguring");
	SECTION("a stop ends the change and stops; stopped, the change is made in place") {
		h.stop();
		h->acknowledgeFade();
		h->completeBarrier();
		CHECK(h->phase() == "stopped");
		CHECK(calls == std::vector<std::string>{"configure g-", "change g-->g10", "change g-->g10"});
	}
	SECTION("a removal") {
		h.remove();
		h->acknowledgeFade();
		h->completeBarrier();
		h->runLoops();
		CHECK(h->gone());
		CHECK(worked.empty());
	}
	SECTION("a disable") {
		h.disable();
		h->acknowledgeFade();
		h->completeBarrier();
		h->runLoops();
		CHECK(h->phase() == "disabled");
		CHECK(worked.empty());
	}
}

TEST_CASE("A start() that threw in the batch that began a change waiting for its drive fails the node start-failed, "
          "with no stop()",
          "[reconfigure]") {
	reset();
	auto owned = running(manual());
	auto& h = *owned;
	h.stop();
	h->acknowledgeFade();
	h->completeBarrier();
	REQUIRE(h->phase() == "stopped");
	script.startThrows = true;
	h.start();
	REQUIRE(h->phase() == "starting"); // its drive in flight
	h.change({{"gain", 10}});          // waits for the drive
	const int stopsBefore = stops;
	h->completeBarrier(); // the drive: start() throws, and the waiting change begins in the same batch
	h->acknowledgeFade();
	h->completeBarrier();
	CHECK(h->phase() == "failed");
	REQUIRE(h->health().reason);
	CHECK(h->health().reason->code == FailureReason::Code::startFailed);
	CHECK(stops == stopsBefore); // no stop() pairs with the start() that threw
}

TEST_CASE("A type's reconfigureDeadline is its spec's", "[reconfigure]") {
	CHECK(makeNodeType<spec, Reconf<spec>>()->reconfigureDeadline() == std::chrono::seconds(10));
	CHECK(makeNodeType<slowSpec, Reconf<slowSpec>>()->reconfigureDeadline() == std::chrono::seconds(30));
}

// ---- A replacement companion, break-first behind a hold the framework takes ----

namespace reconfigure_test {
constexpr auto portRow = setting::required("port", setting::number(1, 65535)).inPlace();
constexpr auto latencyRow = setting::defaulted("latency", setting::number(1, 1000), 100).inPlace();
constexpr auto fromRtp = bank::insideInput<"from_rtp">;
constexpr TypeSpec receivingSpec{.settings = SettingsTable<portRow, latencyRow>{"receiving"},
                                 .banks = Banks<out, side, fromRtp>{}};
constexpr TypeSpec quickSpec{.settings = SettingsTable<portRow, latencyRow>{"quick"},
                             .banks = Banks<out, side, fromRtp>{},
                             .lifecycle = Lifecycle{.reconfigureDeadline = std::chrono::seconds(30)}};

template <const auto& Spec> struct Receiving {
	using Checked = CheckedOf<Spec>;
	static Members<Spec> ports(const Checked&) { return {out(members(1)), side(members(1)), fromRtp(members(1))}; }
	static RtpReceiverShape receiverFor(const Checked& s, AudioFormat format) {
		return RtpReceiverShape{.address = *RtpAddress::parse("0.0.0.0"),
		                        .port = static_cast<std::uint16_t>(s.template get<portRow>()),
		                        .encoding = RtpEncoding::l24,
		                        .rate = format.rate,
		                        .latencyMs = static_cast<std::uint32_t>(s.template get<latencyRow>())};
	}
	Configured configure(CreateOf<Spec>& cx, const Checked& s) {
		cx.attach(fromRtp, receiverFor(s, cx.format()));
		return cx.applied(passThrough(bankPair(fromRtp, out)));
	}
	Changed configure(ChangeOf<Spec>& cx, const Checked&, const Checked& now) {
		cx.attach(fromRtp, receiverFor(now, cx.format()));
		return cx.applied();
	}
};
using Receiver = testing::TypeHarness<receivingSpec, Receiving<receivingSpec>>;

/// A receiving node running on port 5004, its receiver ready.
std::unique_ptr<Receiver> receiving(testing::HarnessOptions options = {}) {
	auto h = std::make_unique<Receiver>(std::move(options));
	REQUIRE_FALSE(h->parse({{"port", 5004}}));
	h->add();
	h->blockReady();
	h->node().ready("from_rtp");
	if (h->phase() != "running") h->node().completeBarrier();
	REQUIRE(h->phase() == "running");
	return h;
}

std::uint16_t portOf(const testing::CompanionFake& fake) { return std::get<RtpReceiverShape>(fake.shape).port; }

std::size_t receiversMade(const testing::NodeHarness& h) {
	return static_cast<std::size_t>(std::ranges::count_if(h.companions(), [](const testing::CompanionFake& one) {
		return one.kind == CompanionKind::rtpReceiver && one.made;
	}));
}
} // namespace reconfigure_test

TEST_CASE("A same-port replacement is made break-first behind a framework hold: held, the old destroyed, then the new "
          "made",
          "[reconfigure][replacement]") {
	auto owned = receiving(manual());
	auto& h = *owned;
	h.change({{"port", 5004}, {"latency", 200}});
	REQUIRE(h->phase() == "reconfiguring");
	CHECK(h.held(out)); // what the bank feeds through the pass-through, held by the framework
	CHECK_FALSE(h.held(side));
	CHECK(receiversMade(h.node()) == 1); // nothing broken before the hold is acknowledged
	h->acknowledgeHolds();
	REQUIRE(receiversMade(h.node()) == 2);
	CHECK(h->companions()[0].destroyed);
	CHECK(std::get<RtpReceiverShape>(h->companions()[1].shape).latencyMs == 200);
	CHECK(h->phase() == "reconfiguring"); // the new one is not ready yet
	CHECK(h->companionList().size() == 1);
	CHECK_FALSE(h->companionList()[0].ready);
	h->ready("from_rtp");
	CHECK(h->phase() == "running");
	CHECK_FALSE(h.held(out));
	CHECK_FALSE(toldPhase(h.node(), NodePhase::stopping));
}

TEST_CASE("A replacement on a new port is made before break: held, made beside the old one, which is listed retiring "
          "until the commit point destroys it",
          "[reconfigure][replacement]") {
	auto owned = receiving(manual());
	auto& h = *owned;
	h.change({{"port", 5006}});
	REQUIRE(h->phase() == "reconfiguring");
	CHECK(h.held(out)); // two receivers into one inside input would mix for a moment
	CHECK(receiversMade(h.node()) == 1);
	h->acknowledgeHolds();
	REQUIRE(receiversMade(h.node()) == 2);
	CHECK_FALSE(h->companions()[0].destroyed); // the old one runs on beside its replacement
	CHECK(portOf(h->companions()[1]) == 5006);
	std::vector<CompanionView> list = h->companionList();
	REQUIRE(list.size() == 2);
	CHECK(list[0].retiring);
	CHECK(list[0].ready);
	CHECK(list[0].name == list[1].name);
	CHECK_FALSE(list[1].retiring);
	CHECK_FALSE(list[1].ready);
	h->ready("from_rtp");
	CHECK(h->phase() == "running");
	CHECK(h->companions()[0].destroyed);
	list = h->companionList();
	REQUIRE(list.size() == 1);
	CHECK_FALSE(list[0].retiring);
	CHECK(list[0].ready);
	CHECK_FALSE(h.held(out));
	CHECK_FALSE(toldPhase(h.node(), NodePhase::stopping));
}

TEST_CASE("A same-port replacement is granted unprobed, and a new port is probed, and a refused one falls back before "
          "anything is broken",
          "[reconfigure][replacement]") {
	SECTION("the same port, the probe refusing it, is its own") {
		auto owned = receiving({.refusedPorts = {}});
		auto& h = *owned;
		h.change({{"port", 5004}});
		h->ready("from_rtp");
		CHECK(h->phase() == "running");
	}
	SECTION("a new port the kernel refuses") {
		auto owned = receiving({.refusedPorts = {"udp:5006"}});
		auto& h = *owned;
		h.change({{"port", 5006}});
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::resourceRefused);
		CHECK(h->companions()[0].destroyed); // by the restart's teardown, not a break
		CHECK(h->phase() == "failed");       // the restart's own claim is refused too
	}
}

TEST_CASE("A replacement lost before the commit point falls back (resource-lost), and a retiring one lost is only said",
          "[reconfigure][replacement]") {
	SECTION("the replacement lost") {
		auto owned = receiving();
		auto& h = *owned;
		h.change({{"port", 5006}});
		REQUIRE(h->phase() == "reconfiguring");
		h->lose("from_rtp", "gone");
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::resourceLost);
	}
	SECTION("the retiring one lost while its hold waits") {
		auto owned = receiving(manual());
		auto& h = *owned;
		h.change({{"port", 5006}});
		REQUIRE(h->phase() == "reconfiguring");
		h->lose("from_rtp", "gone"); // the old receiver, not yet broken
		CHECK(said(h.node(), "ear.retiring-lost"));
		CHECK(h->phase() == "reconfiguring");
		h->acknowledgeHolds();
		h->ready("from_rtp");
		CHECK(h->phase() == "running");
	}
}

TEST_CASE("A change not ready by its deadline falls back: 10 s, or the 30 s its spec declares",
          "[reconfigure][replacement][deadline]") {
	SECTION("10 s by default") {
		auto owned = receiving();
		auto& h = *owned;
		h.change({{"port", 5006}});
		h->advance(std::chrono::milliseconds(9'999));
		CHECK(h->phase() == "reconfiguring");
		h->advance(std::chrono::milliseconds(1));
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::deadline);
		CHECK(said(h.node(), "ear.reconfigure-fell-back"));
	}
	SECTION("30 s declared") {
		testing::TypeHarness<quickSpec, Receiving<quickSpec>> h;
		REQUIRE_FALSE(h.parse({{"port", 5004}}));
		h.add();
		h.blockReady();
		h->ready("from_rtp");
		REQUIRE(h.phase() == "running");
		h.change({{"port", 5006}});
		h->advance(std::chrono::seconds(29));
		CHECK(h.phase() == "reconfiguring");
		h->advance(std::chrono::seconds(1));
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::deadline);
	}
}

TEST_CASE("A newer change that breaks before the older one's flush ran makes only its own replacement, and commits",
          "[reconfigure][replacement][supersede]") {
	auto owned = receiving(manual());
	auto& h = *owned;
	h.change({{"port", 5004}, {"latency", 150}});
	REQUIRE(h->phase() == "reconfiguring");
	// The hold's acknowledgement breaks the first change; the second, on the same port (its claims all the
	// configuration's), reached the main loop before that break's flush ran, so it breaks at once too.
	h->acknowledgeHoldsThenChange({{"port", 5004}, {"latency", 200}});
	CHECK(said(h.node(), "ear.reconfigure-superseded"));
	REQUIRE(receiversMade(h.node()) == 2); // the first change's replacement is never made
	CHECK(h->companions()[0].destroyed);
	const auto& made = h->companion("from_rtp");
	CHECK(std::get<RtpReceiverShape>(made.shape).latencyMs == 200);
	h->ready("from_rtp");
	CHECK(h->phase() == "running");
	CHECK_FALSE(fellBackReason(h.node()));
}

TEST_CASE("Supersession while a replacement is pending makes the newest, keeping the hold",
          "[reconfigure][replacement]") {
	auto owned = receiving();
	auto& h = *owned;
	h.change({{"port", 5006}});
	REQUIRE(h->phase() == "reconfiguring");
	h.change({{"port", 5008}});
	CHECK(said(h.node(), "ear.reconfigure-superseded"));
	CHECK(h.held(out));
	// The abandoned replacement is destroyed, and the old one is the bank's again until the newest commits.
	REQUIRE(receiversMade(h.node()) == 3);
	CHECK_FALSE(h->companions()[0].destroyed);
	CHECK(h->companions()[1].destroyed);
	CHECK(portOf(h->companion("from_rtp")) == 5008);
	h->ready("from_rtp");
	CHECK(h->phase() == "running");
	CHECK(h->companions()[0].destroyed);
}

// ---- whenReady and ConfigureDone (docs/writing-a-node-type.md#whenready-and-configuredone) ----

namespace reconfigure_test {
/// The loop the continuation runs on: what ran on it, in order.
struct Swapper {
	std::vector<std::string> seen;
};
constexpr auto swapper = loop::strand<"swapper", Swapper>;
constexpr TypeSpec continuingSpec{.settings = SettingsTable<portRow, latencyRow>{"continuing"},
                                  .banks = Banks<out, side, fromRtp>{},
                                  .loops = Loops<swapper>{}};

/// How the continuation answers.
enum class Answer { done, fail, drop, keep };
Answer answer = Answer::done;
/// What ran on the loop, over every configuration, in order: "continue <port>", "post <port>".
std::vector<std::string> ran;
/// A token the continuation kept, for the test to answer from its own thread.
std::optional<ConfigureDone> kept;

RtpReceiverShape receiverOf(const CheckedOf<continuingSpec>& s, AudioFormat format) {
	return RtpReceiverShape{.address = *RtpAddress::parse("0.0.0.0"),
	                        .port = static_cast<std::uint16_t>(s.get<portRow>()),
	                        .encoding = RtpEncoding::l24,
	                        .rate = format.rate,
	                        .latencyMs = static_cast<std::uint32_t>(s.get<latencyRow>())};
}

struct Continuing {
	using Checked = CheckedOf<continuingSpec>;
	static Members<continuingSpec> ports(const Checked&) {
		return {out(members(1)), side(members(1)), fromRtp(members(1))};
	}
	Configured configure(CreateOf<continuingSpec>& cx, const Checked& s) {
		cx.attach(fromRtp, receiverOf(s, cx.format()));
		return cx.applied(passThrough(bankPair(fromRtp, out)), cx.state(swapper));
	}
	Changed configure(ChangeOf<continuingSpec>& cx, const Checked&, const Checked& now) {
		cx.attach(fromRtp, receiverOf(now, cx.format()));
		const std::string port = std::to_string(now.get<portRow>());
		cx.post(swapper, [port](Swapper& s) {
			s.seen.push_back("post " + port);
			ran.push_back("post " + port);
		});
		return cx.whenReady(swapper, [port](Swapper& s, ConfigureDone done) {
			s.seen.push_back("continue " + port);
			ran.push_back("continue " + port);
			switch (answer) {
			case Answer::done:
				std::move(done).done();
				break;
			case Answer::fail:
				std::move(done).fail("the client did not start");
				break;
			case Answer::drop:
				break;
			case Answer::keep:
				kept.emplace(std::move(done));
				break;
			}
		});
	}
};
using Continuer = testing::TypeHarness<continuingSpec, Continuing>;

std::unique_ptr<Continuer> continuing(testing::HarnessOptions options = {}, Answer how = Answer::done) {
	answer = how;
	ran.clear();
	kept.reset();
	auto h = std::make_unique<Continuer>(std::move(options));
	REQUIRE_FALSE(h->parse({{"port", 5004}}));
	h->add();
	h->blockReady();
	h->node().ready("from_rtp");
	REQUIRE(h->phase() == "running");
	h->runLoops();
	return h;
}

/// The fallback's detail, as its stopping was told.
std::string fellBackDetail(const testing::NodeHarness& h) {
	for (const NodeHealth& one : h.healths())
		if (one.phase == NodePhase::stopping && one.cause && one.cause->restart == NodeCause::Kind::fellBack)
			return one.cause->detail;
	return {};
}
} // namespace reconfigure_test

TEST_CASE("A continuation runs on the loop named only once every acquisition is ready, then the staged posts, then "
          "the commit point",
          "[reconfigure][continuation]") {
	auto owned = continuing();
	auto& h = *owned;
	h.change({{"port", 5006}});
	h->runLoops();
	CHECK(ran.empty()); // the new receiver is not ready
	CHECK(h->phase() == "reconfiguring");
	h->ready("from_rtp");
	CHECK(h->phase() == "reconfiguring"); // posted, not yet run
	CHECK_FALSE(h->companions()[0].destroyed);
	h->runLoops();
	CHECK(ran == std::vector<std::string>{"continue 5006", "post 5006"});
	CHECK(h->phase() == "running");
	CHECK(h->companions()[0].destroyed); // the replaced receiver goes at the commit point, after done
	CHECK_FALSE(fellBackReason(h.node()));
}

TEST_CASE("A continuation that fails, or drops its token, falls the change back, reason failed",
          "[reconfigure][continuation]") {
	SECTION("fail") {
		auto owned = continuing({}, Answer::fail);
		auto& h = *owned;
		h.change({{"port", 5006}});
		h->ready("from_rtp");
		h->runLoops();
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::failed);
		CHECK(fellBackDetail(h.node()) == "the client did not start");
	}
	SECTION("the token dropped") {
		auto owned = continuing({}, Answer::drop);
		auto& h = *owned;
		h.change({{"port", 5006}});
		h->ready("from_rtp");
		h->runLoops();
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::failed);
		CHECK(fellBackDetail(h.node()) == "the change was dropped");
	}
	CHECK(std::ranges::find(ran, std::string("post 5006")) == ran.end()); // a staged post never runs
}

TEST_CASE("A continuation is destroyed unrun when its change is superseded or refused before the post",
          "[reconfigure][continuation]") {
	SECTION("superseded") {
		auto owned = continuing();
		auto& h = *owned;
		h.change({{"port", 5006}});
		h.change({{"port", 5008}});
		h->ready("from_rtp");
		h->runLoops();
		CHECK(ran == std::vector<std::string>{"continue 5008", "post 5008"});
		CHECK(h->phase() == "running");
	}
	SECTION("a new claim refused") {
		auto owned = continuing({.refusedPorts = {"udp:5006"}});
		auto& h = *owned;
		h.change({{"port", 5006}});
		h->runLoops();
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::resourceRefused);
		CHECK(ran.empty());
	}
}

TEST_CASE("Once posted, a continuation runs even if a removal began, before the goodbye",
          "[reconfigure][continuation]") {
	auto owned = continuing();
	auto& h = *owned;
	h.change({{"port", 5006}});
	h->ready("from_rtp");
	h.remove();
	CHECK(ran == std::vector<std::string>{"continue 5006"}); // its done is ignored: no commit, no staged post
	CHECK(h->gone());
}

TEST_CASE("A newer change after the continuation's post waits for the change to end, then is made",
          "[reconfigure][continuation]") {
	auto owned = continuing({}, Answer::keep);
	auto& h = *owned;
	h.change({{"port", 5006}});
	h->ready("from_rtp");
	h->runLoops();
	REQUIRE(kept);
	h.change({{"port", 5008}});
	CHECK_FALSE(said(h.node(), "ear.reconfigure-superseded"));
	CHECK(h->health().next == std::vector<std::string>{"port"});
	answer = Answer::done;
	std::move(*kept).done(); // from the test's thread: any thread may answer
	kept.reset();
	h->runLoops(); // the answer lands, the change commits; the newer one begins
	CHECK(h->phase() == "reconfiguring");
	CHECK(portOf(h->companion("from_rtp")) == 5008);
	h->ready("from_rtp");
	h->runLoops();
	CHECK(h->phase() == "running");
	CHECK(ran == std::vector<std::string>{"continue 5006", "post 5006", "continue 5008", "post 5008"});
}

// ---- A sink paused while the type swaps what its consumer writes into (docs/writing-a-node-type.md#pausing-a-sink) ----

namespace reconfigure_test {
constexpr auto tap = bank::insideOutput<"tap">;

/// A consumer that records each chunk's position, and its finish.
struct Tap final : SinkConsumer {
	std::vector<std::uint64_t> positions;
	bool finished = false;
	void consume(const Chunk& chunk) override { positions.push_back(chunk.position); }
	void finish() override { finished = true; }
};
/// Read by the test once the state is gone.
bool tapFinished = false;
/// The loop's state: the sink's consumer, and what attaching the sink returned.
struct Writer {
	Writer(SinkControl& c, Loop& l) : control(c), loop(l) {}
	~Writer() { tapFinished = tap.finished; }
	Tap tap;
	SinkControl& control;
	Loop& loop;
};
constexpr auto writer = loop::dedicated<"writer", Writer>;
constexpr TypeSpec pausingSpec{
	.settings = SettingsTable<gainRow>{"pausing"}, .banks = Banks<in, tap>{}, .loops = Loops<writer>{}};

/// The pause's token and the state, for the test to answer and to read on its own thread.
std::optional<ConfigureDone> pausedDone;
Writer* pausedState = nullptr;

struct Pausing {
	using Checked = CheckedOf<pausingSpec>;
	static Members<pausingSpec> ports(const Checked&) { return {in(members(1)), tap(members(1))}; }
	Configured configure(CreateOf<pausingSpec>& cx, const Checked&) {
		SinkControl& control = cx.attach(tap, SinkShape{.frames = 4096}, writer, &Writer::tap);
		auto state = cx.state(writer, control, cx.executor(writer));
		return cx.applied(passThrough(bankPair(in, tap)), std::move(state));
	}
	Changed configure(ChangeOf<pausingSpec>& cx, const Checked&, const Checked&) {
		return cx.whenReady(writer, [](Writer& w, ConfigureDone done) {
			w.control.pause(w.loop, [&w, done = std::move(done)]() mutable {
				pausedState = &w; // the consumer is the loop's now: a swap would go here
				pausedDone.emplace(std::move(done));
			});
		});
	}
};
using Pauser = testing::TypeHarness<pausingSpec, Pausing>;
} // namespace reconfigure_test

TEST_CASE("A sink paused from a loop posts paused, discards and counts what the block taps, and after resume consumes "
          "again with the gap in its position",
          "[reconfigure][pause]") {
	pausedDone.reset();
	pausedState = nullptr;
	Pauser h;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add();
	h.blockReady();
	h->runLoops();
	REQUIRE(h.phase() == "running");
	h->process();
	h.change({{"gain", 3}});
	h->runLoops(); // the continuation pauses the sink; its paused runs on the loop
	REQUIRE(pausedState != nullptr);
	CHECK(h.paused(tap));
	h->process();
	h->process();
	CHECK(h.pausedFrames(tap) == 512);
	CHECK(pausedState->tap.positions == std::vector<std::uint64_t>{0});
	pausedState->control.resume();
	std::move(*pausedDone).done();
	pausedDone.reset();
	h->runLoops();
	CHECK(h.phase() == "running");
	CHECK_FALSE(h.paused(tap));
	h->process();
	CHECK(pausedState->tap.positions == std::vector<std::uint64_t>{0, 768});
	SECTION("a paused sink at teardown is drained by discarding, then finished") {
		h.change({{"gain", 4}});
		h->runLoops();
		REQUIRE(h.paused(tap));
		tapFinished = false;
		h.remove();
		CHECK(tapFinished);
		pausedDone.reset(); // its change ended: the answer lands nowhere
	}
}

// ---- Sinks and sources replaced in a change (docs/writing-a-node-type.md#sinks-and-sources-in-a-change) ----

namespace reconfigure_test {
constexpr auto ringRow = setting::defaulted("ring", setting::number(1, 99), 1).inPlace();
constexpr auto fromSource = bank::insideInput<"from_source">;
constexpr TypeSpec ringSpec{.settings = SettingsTable<ringRow>{"rings"}, .banks = Banks<in, out, tap, fromSource>{}};

/// What each consumer saw, by the ring setting it was made for: owned by its sink, so read here once it is gone.
std::map<int, std::vector<std::uint64_t>> tapped;
std::map<int, bool> began;
std::map<int, bool> finished;
/// Each source's writer, by the ring setting it was made for.
std::map<int, SourceWriter*> writers;

struct Positions final : SinkConsumer {
	explicit Positions(int r) : ring(r) {}
	int ring;
	void begin() override { began[ring] = true; }
	void consume(const Chunk& chunk) override { tapped[ring].push_back(chunk.position); }
	void finish() override { finished[ring] = true; }
};

/// What the change version replaces, set by each test.
struct RingScript {
	bool sink = true;
	bool source = false;
	bool restart = false;
	bool hold = false;
};
RingScript ringScript;

struct Rings {
	using Checked = CheckedOf<ringSpec>;
	static Members<ringSpec> ports(const Checked&) {
		return {in(members(1)), out(members(1)), tap(members(1)), fromSource(members(1))};
	}
	static int ringOf(const Checked& s) { return static_cast<int>(s.get<ringRow>()); }
	Configured configure(CreateOf<ringSpec>& cx, const Checked& s) {
		cx.attach(tap, SinkShape{.frames = 4096}, std::make_unique<Positions>(ringOf(s)));
		writers[ringOf(s)] = &cx.attach(fromSource, SourceShape{.frames = 1024});
		return cx.applied(passThrough(bankPair(in, tap), bankPair(fromSource, out)));
	}
	Changed configure(ChangeOf<ringSpec>& cx, const Checked&, const Checked& now) {
		if (ringScript.sink) cx.attach(tap, SinkShape{.frames = 4096}, std::make_unique<Positions>(ringOf(now)));
		if (ringScript.source) writers[ringOf(now)] = &cx.attach(fromSource, SourceShape{.frames = 1024});
		if (ringScript.hold) cx.holdAll();
		if (ringScript.restart) return Changed::restart("cannot take it");
		return cx.applied();
	}
};
using Ringed = testing::TypeHarness<ringSpec, Rings>;

void resetRings(RingScript asked = {}) {
	ringScript = asked;
	tapped.clear();
	began.clear();
	finished.clear();
	writers.clear();
}

/// A ringed node running, its sink begun.
std::unique_ptr<Ringed> ringed(testing::HarnessOptions options = {}) {
	auto h = std::make_unique<Ringed>(std::move(options));
	REQUIRE_FALSE(h->parse(NodeSettings::object()));
	h->add();
	h->blockReady();
	if ((*h)->companion("tap").ready == false) (*h)->ready("tap");
	if (h->phase() != "running") (*h)->completeBarrier();
	REQUIRE(h->phase() == "running");
	return h;
}

std::vector<float> filled(std::size_t frames, float value) { return std::vector<float>(frames, value); }
} // namespace reconfigure_test

TEST_CASE("A sink replaced in a change takes the cycles from the commit's invoke on, the old one those before it, "
          "exactly, and the old one drains to its own consumer and finishes after the swap",
          "[reconfigure][rings]") {
	resetRings();
	auto owned = ringed(manual());
	auto& h = *owned;
	h->process();
	h->process();
	h.change({{"ring", 2}});
	CHECK(began[2]);                     // its consumer began beside the old one's
	CHECK(h->swaps() == 1);              // the commit's invoke, with the same code: the tap moves in it
	CHECK(h.phase() == "reconfiguring"); // until the invoke completes
	h->process();
	h->process();
	CHECK_FALSE(finished[1]); // the old one still draining: nothing finishes it before the swap completed
	h->completeBarrier();
	CHECK(finished[1]);
	CHECK(h.phase() == "running");
	h->process();
	CHECK(tapped[1] == std::vector<std::uint64_t>{0, 256});
	CHECK(tapped[2] == std::vector<std::uint64_t>{512, 768, 1024});
	CHECK_FALSE(finished[2]);
	REQUIRE(h->companionList().size() == 2); // the sink and the source, each once
	CHECK_FALSE(h->companionList()[0].retiring);
}

TEST_CASE("A teardown after a sink was replaced closes the replacement alone and ends, the old sink counted nowhere",
          "[reconfigure][rings]") {
	resetRings();
	auto owned = ringed();
	auto& h = *owned;
	h.change({{"ring", 2}});
	h.change({{"ring", 3}});
	REQUIRE(h.phase() == "running");
	CHECK(finished[1]);
	CHECK(finished[2]);
	CHECK_FALSE(finished[3]);
	h.remove();
	CHECK(finished[3]);
	CHECK(h.gone());
}

TEST_CASE("A sink replaced in a change from stopped is swapped at once, the old one finished at the commit point",
          "[reconfigure][rings]") {
	resetRings();
	auto owned = ringed();
	auto& h = *owned;
	h.stop();
	REQUIRE(h.phase() == "stopped");
	h.change({{"ring", 2}});
	CHECK(h.phase() == "stopped");
	CHECK(finished[1]);
	CHECK(h->swaps() == 0);
	h.start();
	REQUIRE(h.phase() == "running");
	h->process();
	CHECK(tapped[1].empty());
	CHECK(tapped[2].size() == 1);
}

TEST_CASE("A replacement sink is listed beside the one it replaces, which is retiring, until its consumer began and "
          "the commit swapped them",
          "[reconfigure][rings]") {
	resetRings();
	auto owned = ringed({.holdSinks = true});
	auto& h = *owned;
	h.change({{"ring", 2}});
	REQUIRE(h.phase() == "reconfiguring"); // the commit waits for the replacement's consumer
	std::vector<CompanionView> list = h->companionList();
	REQUIRE(list.size() == 3);
	CHECK(list[0].kind == CompanionKind::sink);
	CHECK(list[0].retiring);
	CHECK(list[0].ready);
	CHECK(list[1].name == list[0].name);
	CHECK_FALSE(list[1].retiring);
	CHECK_FALSE(list[1].ready);
	h->ready("tap");
	CHECK(h.phase() == "running");
	list = h->companionList();
	REQUIRE(list.size() == 2);
	CHECK_FALSE(list[0].retiring);
	CHECK(list[0].ready);
	CHECK(finished[1]);
}

TEST_CASE("A replacement sink of a change that ends uncommitted is closed, never tapped, and the old one taps on",
          "[reconfigure][rings]") {
	SECTION("superseded before its consumer began: closed with nothing to finish") {
		resetRings();
		auto owned = ringed({.holdSinks = true});
		auto& h = *owned;
		h.change({{"ring", 2}});
		REQUIRE(h.phase() == "reconfiguring");
		h.change({{"ring", 3}});
		CHECK(h->companions()[2].destroyed); // ring 2's sink
		CHECK_FALSE(began[2]);
		h->ready("tap");
		CHECK(h.phase() == "running");
		h->process();
		CHECK(tapped[3].size() == 1);
		CHECK(tapped[2].empty());
		CHECK(finished[1]);
	}
	SECTION("superseded after its consumer began: finished, never tapped") {
		resetRings({.hold = true});
		auto owned = ringed(manual());
		auto& h = *owned;
		h.change({{"ring", 2}});
		REQUIRE(h.phase() == "reconfiguring"); // the hold not yet acknowledged
		CHECK(began[2]);
		h.change({{"ring", 3}});
		CHECK(finished[2]);
		CHECK_FALSE(finished[1]);
		h->acknowledgeHolds();
		h->completeBarrier();
		CHECK(h.phase() == "running");
		CHECK(finished[1]);
		h->process();
		CHECK(tapped[2].empty());
		CHECK(tapped[3].size() == 1);
	}
	SECTION("fallen back: closed unbegun, and the restart's own sink takes the cycles") {
		resetRings({.restart = true});
		auto owned = ringed();
		auto& h = *owned;
		h->process();
		h.change({{"ring", 2}});
		REQUIRE(h->companions().size() >= 3);
		CHECK(h->companions()[2].kind == CompanionKind::sink); // the replacement, closed when the change fell back
		CHECK(h->companions()[2].destroyed);
		CHECK(h->companions()[2].chunks.empty());
		CHECK(finished[1]);
		CHECK(h.phase() == "running"); // restarted with ring 2: its configure's own sink
		h->process();
		CHECK(tapped[1] == std::vector<std::uint64_t>{0});
		CHECK(tapped[2].size() == 1); // the restart's sink, ring 2's too
	}
}

TEST_CASE("A source replaced in a change plays from the commit's invoke on, the old one's writer closed at the commit "
          "point, so two never play into one bank",
          "[reconfigure][rings]") {
	resetRings({.sink = false, .source = true});
	auto owned = ringed();
	auto& h = *owned;
	REQUIRE(h->feed("from_source", filled(1024, 1.0F)) == 1024);
	h->process();
	CHECK(h.output(out, 0).back() == 1.0F);
	SourceWriter* old = writers[1];
	h.change({{"ring", 2}});
	REQUIRE(h.phase() == "running");
	CHECK(old->closed());
	CHECK_FALSE(writers[2]->closed());
	REQUIRE(h->feed("from_source", filled(1024, 0.5F)) == 1024);
	h->process();
	CHECK(h.output(out, 0).back() == 0.5F); // the old one's ring still held 768 frames of 1.0: never played
	CHECK(h->swaps() == 1);
}

namespace reconfigure_test {
/// A loop's state with two consumers: the change may hand the sink's replacement the other one, never the same one.
struct TwoTaps {
	Tap a;
	Tap b;
};
constexpr auto taps = loop::strand<"taps", TwoTaps>;
constexpr TypeSpec twoTapSpec{
	.settings = SettingsTable<ringRow>{"two_taps"}, .banks = Banks<in, tap>{}, .loops = Loops<taps>{}};
struct TwoTapping {
	using Checked = CheckedOf<twoTapSpec>;
	static Members<twoTapSpec> ports(const Checked&) { return {in(members(1)), tap(members(1))}; }
	Configured configure(CreateOf<twoTapSpec>& cx, const Checked&) {
		cx.attach(tap, SinkShape{.frames = 4096}, taps, &TwoTaps::a);
		return cx.applied(passThrough(bankPair(in, tap)), cx.state(taps));
	}
	Changed configure(ChangeOf<twoTapSpec>& cx, const Checked&, const Checked& now) {
		if (now.get<ringRow>() == 2) cx.attach(tap, SinkShape{.frames = 4096}, taps, &TwoTaps::b);
		else cx.attach(tap, SinkShape{.frames = 4096}, taps, &TwoTaps::a);
		return cx.applied();
	}
};
using TwoTapped = testing::TypeHarness<twoTapSpec, TwoTapping>;
} // namespace reconfigure_test

TEST_CASE("A replacement sink consumed by a loop's state takes another consumer of it, never the one it replaces",
          "[reconfigure][rings]") {
	TwoTapped h;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add();
	h.blockReady();
	h->runLoops();
	REQUIRE(h.phase() == "running");
	SECTION("another member: swapped") {
		h.change({{"ring", 2}});
		CHECK(h.phase() == "running");
		CHECK(h->swaps() == 1);
	}
	SECTION("the same member: a broken rule, the node lost") {
		h.change({{"ring", 3}});
		CHECK(h.phase() == "failed");
		REQUIRE(h.health().reason);
		CHECK(h.health().reason->code == FailureReason::Code::lost);
		CHECK(h.health().reason->detail.find("is consumed by the consumer it replaces") != std::string::npos);
	}
}

// ---- Sound cards replaced in a change, made before break or break-first (docs/writing-a-node-type.md#make-before-break)

namespace reconfigure_test {
constexpr auto cardRow = setting::required("card", setting::text<CardId::parse>("an ALSA card id")).inPlace();
constexpr auto toCard = bank::insideOutput<"to_card">;
constexpr auto fromCard = bank::insideInput<"from_card">;
constexpr TypeSpec cardSpec{.settings = SettingsTable<cardRow>{"cards"}, .banks = Banks<in, out, toCard, fromCard>{}};

/// How the change orders the playback card's replacement, set by each test.
Order cardOrder = Order::makeFirst;

struct Cards {
	using Checked = CheckedOf<cardSpec>;
	static Members<cardSpec> ports(const Checked&) {
		return {in(members(1)), out(members(1)), toCard(members(1)), fromCard(members(1))};
	}
	static SoundCardShape cardOf(const Checked& s) { return SoundCardShape{s.get<cardRow>()}; }
	Configured configure(CreateOf<cardSpec>& cx, const Checked& s) {
		cx.attach(toCard, cardOf(s));
		cx.attach(fromCard, cardOf(s));
		return cx.applied(passThrough(bankPair(in, toCard), bankPair(fromCard, out)));
	}
	Changed configure(ChangeOf<cardSpec>& cx, const Checked&, const Checked& now) {
		cx.attach(toCard, cardOf(now), cardOrder);
		return cx.applied();
	}
};
using Carded = testing::TypeHarness<cardSpec, Cards>;

std::unique_ptr<Carded> carded() {
	auto h = std::make_unique<Carded>(manual());
	REQUIRE_FALSE(h->parse({{"card", "Pro24"}}));
	h->add();
	h->blockReady();
	h->ready(toCard);
	h->ready(fromCard);
	if (h->phase() != "running") (*h)->completeBarrier();
	REQUIRE(h->phase() == "running");
	return h;
}

std::size_t playbacksMade(const testing::NodeHarness& h) {
	return static_cast<std::size_t>(std::ranges::count_if(h.companions(), [](const testing::CompanionFake& one) {
		return one.kind == CompanionKind::cardPlayback && one.made;
	}));
}
} // namespace reconfigure_test

TEST_CASE("A playback card replaced in a change holds its own bank, and is made before break unless the type orders "
          "break-first",
          "[reconfigure][replacement][card]") {
	SECTION("made before break: the new card beside the old one, which is retiring until the commit point") {
		cardOrder = Order::makeFirst;
		auto owned = carded();
		auto& h = *owned;
		h.change({{"card", "Other"}});
		REQUIRE(h.phase() == "reconfiguring");
		CHECK(h.held(toCard)); // what the card plays, held so neither card clicks
		CHECK_FALSE(h.held(out));
		CHECK(playbacksMade(h.node()) == 1);
		h->acknowledgeHolds();
		REQUIRE(playbacksMade(h.node()) == 2);
		CHECK_FALSE(h->companions()[0].destroyed);
		std::vector<CompanionView> list = h->companionList();
		REQUIRE(list.size() == 3);
		CHECK(list[0].retiring);
		CHECK(list[0].kind == CompanionKind::cardPlayback);
		CHECK_FALSE(list[1].retiring);
		CHECK_FALSE(list[1].ready);
		h->ready("to_card");
		CHECK(h.phase() == "running");
		CHECK(h->companions()[0].destroyed);
		CHECK(h->companionList().size() == 2);
		CHECK_FALSE(h.held(toCard));
	}
	SECTION("break-first: the old card destroyed and the new one made in one flush, once the hold is acknowledged") {
		cardOrder = Order::breakFirst;
		auto owned = carded();
		auto& h = *owned;
		h.change({{"card", "Pro24b"}});
		REQUIRE(h.phase() == "reconfiguring");
		CHECK(h.held(toCard));
		CHECK(playbacksMade(h.node()) == 1);
		h->acknowledgeHolds();
		REQUIRE(playbacksMade(h.node()) == 2);
		CHECK(h->companions()[0].destroyed);
		REQUIRE(h->companionList().size() == 2);
		CHECK_FALSE(h->companionList()[0].retiring);
		h->ready("to_card");
		CHECK(h.phase() == "running");
		CHECK_FALSE(h.held(toCard));
		CHECK_FALSE(toldPhase(h.node(), NodePhase::stopping));
	}
	cardOrder = Order::makeFirst;
}

// ---- A recording replaced in a change (docs/writing-a-node-type.md#recordings-in-a-change) ----

namespace reconfigure_test {
inline std::optional<std::string> anyPath(std::string_view value) { return std::string(value); }
constexpr auto pathRow = setting::required("path", setting::text<anyPath>("a recording's path")).inPlace();
constexpr TypeSpec recordingSpec{.settings = SettingsTable<pathRow>{"recorded"}, .banks = Banks<in, tap>{}};

/// Writes one byte per chunk into its recording.
struct Bytes final : SinkConsumer {
	explicit Bytes(Recording& r) : recording(r) {}
	Recording& recording;
	void consume(const Chunk&) override { static_cast<void>(::write(recording.descriptor(), "x", 1)); }
	void finish() override {}
};

struct Recorded {
	using Checked = CheckedOf<recordingSpec>;
	static Members<recordingSpec> ports(const Checked&) { return {in(members(1)), tap(members(1))}; }
	Configured configure(CreateOf<recordingSpec>& cx, const Checked& s) {
		recording = &cx.recording({s.get<pathRow>()});
		cx.attach(tap, SinkShape{.frames = 4096}, std::make_unique<Bytes>(*recording));
		return cx.applied(passThrough(bankPair(in, tap)));
	}
	Changed configure(ChangeOf<recordingSpec>& cx, const Checked&, const Checked& now) {
		Recording& next = cx.recording(*recording, {now.get<pathRow>()});
		cx.attach(tap, SinkShape{.frames = 4096}, std::make_unique<Bytes>(next));
		recording = &next; // read only by the next change
		return cx.applied();
	}
	Recording* recording = nullptr;
};
using Recorder = testing::TypeHarness<recordingSpec, Recorded>;

std::unique_ptr<Recorder> recorder(testing::HarnessOptions options = {}) {
	auto h = std::make_unique<Recorder>(std::move(options));
	REQUIRE_FALSE(h->parse({{"path", "a.wav"}}));
	h->add();
	h->blockReady();
	h->runIo();
	REQUIRE(h->phase() == "running");
	return h;
}

std::size_t indexIn(const std::vector<std::string>& jobs, std::string_view job) {
	return static_cast<std::size_t>(std::ranges::find(jobs, job) - jobs.begin());
}
} // namespace reconfigure_test

TEST_CASE("A recording replaced in a change is opened beside the old one before the replacement sink begins, and the "
          "old one is closed once the old sink finished",
          "[reconfigure][recording]") {
	auto owned = recorder({.holdIo = true});
	auto& h = *owned;
	h.process();
	h.process();
	h.change({{"path", "b.wav"}});
	REQUIRE(h.phase() == "reconfiguring"); // its open waits on the I/O executor
	CHECK(h.openRecordings() == 1);
	h.runIo();
	CHECK(h.phase() == "running");
	h.runIo(); // the old one's close, posted once its sink finished
	CHECK(h.openRecordings() == 1);
	const std::vector<std::string>& jobs = h->ioJobs();
	CHECK(indexIn(jobs, "open recording b.wav") < indexIn(jobs, "close recording a.wav"));
	CHECK(indexIn(jobs, "close recording a.wav") < jobs.size());
	h.process();
	CHECK(h.recordingBytes("a.wav") == "xx");
	CHECK(h.recordingBytes("b.wav") == "x");
}

TEST_CASE("A recording a change cannot open falls the change back (resource-refused), and one replaced by its own "
          "path is a broken rule",
          "[reconfigure][recording]") {
	SECTION("refused") {
		auto owned = recorder();
		auto& h = *owned;
		h.failRecordingOpen("b.wav", "no room");
		h.change({{"path", "b.wav"}});
		CHECK(fellBackReason(h.node()) == NodeCause::FellBack::resourceRefused);
		CHECK(h.phase() == "failed"); // the restart cannot open it either
		CHECK(h.openRecordings() == 0);
	}
	SECTION("its own path") {
		auto owned = recorder();
		auto& h = *owned;
		h.change({{"path", "./a.wav"}});
		CHECK(h.phase() == "failed");
		REQUIRE(h.health().reason);
		CHECK(h.health().reason->code == FailureReason::Code::lost);
	}
}
