// A type's actions (docs/writing-a-node-type.md#actions): a trigger into the code through the configuration's realtime
// channels, a call into a loop's state with its arguments checked by a table, and what NodeHost answers each, on a test
// type by the type harness over a real NodeHost.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

namespace actions_test {

using namespace ear;
using namespace std::chrono_literals;

struct Counter;
constexpr auto gainRow = setting::optional("gain", setting::number(0, 100)).inPlace();
constexpr SettingsTable<gainRow> table{"actor"};
constexpr auto out = bank::output<"out">;
constexpr auto work = loop::strand<"work", Counter>;
constexpr auto amountRow = setting::required("amount", setting::number(1, 10));
constexpr SettingsTable<amountRow> addArgs{"add"};
constexpr SettingsTable<> noArgs{"refuse"};
constexpr auto reset = action::trigger<"reset">("clear the latch");
constexpr auto add = action::call<"add", addArgs, work>("add to the count");
constexpr auto refuse = action::call<"refuse", noArgs, work>("refuse, always");
constexpr TypeSpec spec{.settings = table,
                        .banks = Banks<out>{},
                        .loops = Loops<work>{},
                        .actions = ActionTable<reset, add, refuse>{}};
constexpr TypeSpec statusAfterNothing{.settings = table, .banks = Banks<out>{}};

// Every spelling: .actions alone after each member before it, before .logCodes, and all together, deduce the rest.
constexpr auto file = store::file<"key">({.maxBytes = 64});
constexpr auto started = event::of<"started">(EventSeverity::info, "started");
constexpr auto overrun = log::code<"overrun">(LogLevel::warn, "overran by {us} us", arg::number<"us">(0, 1'000'000));
constexpr auto ready = status::flag<"ready">(writer::loop(work));
constexpr TypeSpec actionsAfterBanks{.settings = table, .banks = Banks<out>{}, .actions = ActionTable<reset>{}};
constexpr TypeSpec actionsAfterStores{
	.settings = table, .banks = Banks<out>{}, .stores = Stores<file>{}, .actions = ActionTable<reset>{}};
constexpr TypeSpec actionsAfterLoops{
	.settings = table, .banks = Banks<out>{}, .loops = Loops<work>{}, .actions = ActionTable<add>{}};
constexpr TypeSpec actionsAfterLifecycle{.settings = table,
                                         .banks = Banks<out>{},
                                         .lifecycle = {.reconfigureDeadline = std::chrono::seconds(5)},
                                         .actions = ActionTable<reset>{}};
constexpr TypeSpec actionsAfterStatus{.settings = table,
                                      .banks = Banks<out>{},
                                      .loops = Loops<work>{},
                                      .status = StatusTable<ready>{},
                                      .actions = ActionTable<reset>{}};
constexpr TypeSpec actionsAfterEvents{
	.settings = table, .banks = Banks<out>{}, .events = EventTable<started>{}, .actions = ActionTable<reset>{}};
constexpr TypeSpec logCodesAfterActions{
	.settings = table, .banks = Banks<out>{}, .actions = ActionTable<reset>{}, .logCodes = LogCodes<overrun>{}};
constexpr TypeSpec whole{.settings = table,
                         .banks = Banks<out>{},
                         .stores = Stores<file>{},
                         .loops = Loops<work>{},
                         .lifecycle = {},
                         .status = StatusTable<ready>{},
                         .events = EventTable<started>{},
                         .actions = ActionTable<reset, add, refuse>{},
                         .logCodes = LogCodes<overrun>{}};

TEST_CASE("Every spelling of a spec deduces with actions", "[actions]") {
	STATIC_REQUIRE(ActionsOf<actionsAfterBanks>::count == 1);
	STATIC_REQUIRE(ActionsOf<actionsAfterStores>::count == 1);
	STATIC_REQUIRE(ActionsOf<actionsAfterLoops>::count == 1);
	STATIC_REQUIRE(ActionsOf<actionsAfterLifecycle>::count == 1);
	STATIC_REQUIRE(ActionsOf<actionsAfterStatus>::count == 1);
	STATIC_REQUIRE(ActionsOf<actionsAfterEvents>::count == 1);
	STATIC_REQUIRE(ActionsOf<logCodesAfterActions>::count == 1);
	STATIC_REQUIRE(LogCodesOf<logCodesAfterActions>::count == 1);
	STATIC_REQUIRE(ActionsOf<whole>::count == 3);
	STATIC_REQUIRE(ActionsOf<whole>::triggers == 1);
	STATIC_REQUIRE(ActionsOf<whole>::indexOf<refuse> == 2);
	STATIC_REQUIRE(ActionsOf<whole>::triggerIndexOf<reset> == 0);
	STATIC_REQUIRE(ActionsOf<spec>::declared[1].loop == "work");
	STATIC_REQUIRE(ActionsOf<statusAfterNothing>::count == 0);
}

/// How many times a code saw the trigger, and which code (a code's own number) saw it last.
int seen = 0;
int seenBy = 0;
int codesMade = 0;
/// The count the calls added to, on the loop's state.
std::int64_t counted = 0;

struct Code {
	int number = ++codesMade;
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (const auto member : io[out]) std::ranges::fill(member, 0.0F);
		if (io.triggered<reset>()) {
			++seen;
			seenBy = number;
		}
		// Asked twice in one block: true once.
		if (io.triggered<reset>()) ++seen;
	}
};
struct Counter {
	std::expected<void, std::string> act(Invoked<add>, const CheckedOf<addArgs>& args) {
		counted += args.get<amountRow>();
		return {};
	}
	std::expected<void, std::string> act(Invoked<refuse>, const CheckedOf<noArgs>&) {
		return std::unexpected(std::string("nothing to refuse with"));
	}
};
struct Actor {
	static Members<spec> ports(const CheckedOf<spec>&) { return {out(members("mono"))}; }
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
		return cx.applied(Code{}, cx.state(work));
	}
	/// A change of gain in place, answered with a new code: swapped in at the commit point.
	Changed configure(ChangeOf<spec>& cx, const CheckedOf<spec>&, const CheckedOf<spec>&) { return cx.applied(Code{}); }
};
using Harness = testing::TypeHarness<spec, Actor>;

void running(Harness& h) {
	seen = 0;
	seenBy = 0;
	counted = 0;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add();
	h.blockReady();
	INFO((h.health().reason ? h.health().reason->detail : std::string("no reason")));
	REQUIRE(h.phase() == "running");
}

ActionRefusal::Code codeOf(const ActionReply& reply) {
	REQUIRE_FALSE(reply);
	return reply.error().code;
}

TEST_CASE("The schema lists a type's actions in declaration order, a call with its arguments' rows", "[actions]") {
	const auto type = makeNodeType<spec, Actor>();
	const TypeSchema schema = type->schema();
	REQUIRE(schema.actions.size() == 3);
	CHECK(schema.actions[0].name == "reset");
	CHECK(schema.actions[0].target == ActionDeclaration::Target::code);
	CHECK(schema.actions[0].help == "clear the latch");
	CHECK(schema.actions[0].args.empty());
	CHECK(schema.actions[1].name == "add");
	CHECK(schema.actions[1].target == ActionDeclaration::Target::loop);
	CHECK(schema.actions[1].loop == "work");
	REQUIRE(schema.actions[1].args.size() == 1);
	CHECK(schema.actions[1].args[0]->key == "amount");
}

TEST_CASE("Several triggers before a block are seen once, in the next block", "[actions]") {
	Harness h;
	running(h);
	CHECK(h.trigger<reset>());
	CHECK(h.trigger<reset>());
	CHECK(h.trigger<reset>());
	CHECK(seen == 0);
	h.process();
	CHECK(seen == 1);
	h.process();
	CHECK(seen == 1);
	CHECK(h.trigger<reset>());
	h.process();
	CHECK(seen == 2);
}

TEST_CASE("A trigger the old code did not see is seen once by the code swapped in", "[actions][swap]") {
	Harness h;
	running(h);
	const int first = codesMade;
	CHECK(h.trigger<reset>());
	h.change({{"gain", 10}});
	REQUIRE(h->swaps() == 1);
	h.process();
	h.process();
	CHECK(seen == 1);
	CHECK(seenBy == first + 1);
	// One the old code saw is not seen again by the next.
	CHECK(h.trigger<reset>());
	h.process();
	h.change({{"gain", 20}});
	h.process();
	CHECK(seen == 2);
}

TEST_CASE("A trigger is refused not-running unless the code drives", "[actions]") {
	Harness h;
	running(h);
	REQUIRE(h.stop());
	REQUIRE(h.phase() == "stopped");
	const ActionReply reply = h.trigger<reset>();
	REQUIRE(codeOf(reply) == ActionRefusal::Code::notRunning);
	CHECK(reply.error().phase == NodePhase::stopped);
}

TEST_CASE("A call runs its handler on its loop with the checked arguments, and answers with it", "[actions]") {
	Harness h;
	running(h);
	const testing::ActionAnswer added = h.invoke<add>({{"amount", 3}});
	REQUIRE(added.answered());
	CHECK(added.reply());
	CHECK(counted == 3);
	const testing::ActionAnswer refused = h.invoke<refuse>();
	REQUIRE(refused.answered());
	REQUIRE(codeOf(refused.reply()) == ActionRefusal::Code::failed);
	CHECK(refused.reply().error().detail == "nothing to refuse with");
}

TEST_CASE("A call is taken while the node is stopped", "[actions]") {
	Harness h;
	running(h);
	REQUIRE(h.stop());
	const testing::ActionAnswer added = h.invoke<add>({{"amount", 2}});
	REQUIRE(added.answered());
	CHECK(added.reply());
	CHECK(counted == 2);
}

TEST_CASE("A call is refused not-running before the node is configured", "[actions]") {
	Harness h(Harness::Options{.holdLoops = true});
	counted = 0;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add();
	h.blockReady();
	REQUIRE(h.phase() == "configuring");
	const testing::ActionAnswer early = h.invoke<add>({{"amount", 1}});
	REQUIRE(early.answered());
	REQUIRE(codeOf(early.reply()) == ActionRefusal::Code::notRunning);
	CHECK(early.reply().error().phase == NodePhase::configuring);
	h.loopsReady();
	const testing::ActionAnswer later = h.invoke<add>({{"amount", 1}});
	REQUIRE(later.answered());
	CHECK(later.reply());
	CHECK(counted == 1);
}

TEST_CASE("An unknown action, and arguments the table refuses, are refused before anything runs", "[actions]") {
	Harness h;
	running(h);
	REQUIRE(codeOf(h->invoke("forget").reply()) == ActionRefusal::Code::unknownAction);
	const testing::ActionAnswer missing = h->invoke("add", NodeSettings::object());
	REQUIRE(codeOf(missing.reply()) == ActionRefusal::Code::invalidArgs);
	CHECK(missing.reply().error().key == "amount");
	const testing::ActionAnswer outside = h->invoke("add", {{"amount", 11}});
	REQUIRE(codeOf(outside.reply()) == ActionRefusal::Code::invalidArgs);
	CHECK(outside.reply().error().key == "amount");
	CHECK_FALSE(outside.reply().error().detail.empty());
	const testing::ActionAnswer extra = h->invoke("reset", {{"hard", true}});
	REQUIRE(codeOf(extra.reply()) == ActionRefusal::Code::invalidArgs);
	CHECK(extra.reply().error().key == "hard");
	h.runLoops();
	CHECK(counted == 0);
}

TEST_CASE("A queued call is answered restarted by a restart, and stopping by a stop of ear", "[actions]") {
	SECTION("restart") {
		Harness h;
		running(h);
		const testing::ActionAnswer queued = h->invoke("add", {{"amount", 4}});
		REQUIRE_FALSE(queued.answered());
		REQUIRE(h.restart());
		REQUIRE(queued.answered());
		CHECK(codeOf(queued.reply()) == ActionRefusal::Code::restarted);
		CHECK(counted == 0);
	}
	SECTION("stopAll") {
		Harness h;
		running(h);
		const testing::ActionAnswer queued = h->invoke("add", {{"amount", 4}});
		REQUIRE_FALSE(queued.answered());
		h->stopAll();
		REQUIRE(queued.answered());
		CHECK(codeOf(queued.reply()) == ActionRefusal::Code::stopping);
		CHECK(counted == 0);
	}
}

TEST_CASE("A call not answered in 10 s is answered timeout, and its late answer is dropped", "[actions]") {
	Harness h;
	running(h);
	const testing::ActionAnswer queued = h->invoke("add", {{"amount", 5}});
	h->advance(actionDeadline - 1ms);
	REQUIRE_FALSE(queued.answered());
	h->advance(1ms);
	REQUIRE(queued.answered());
	CHECK(codeOf(queued.reply()) == ActionRefusal::Code::timeout);
	h.runLoops();
	CHECK(counted == 5);
	CHECK(codeOf(queued.reply()) == ActionRefusal::Code::timeout);
}

TEST_CASE("An action on a node the host does not have is refused unknown-node", "[actions]") {
	Harness h;
	running(h);
	h.remove();
	REQUIRE(h.gone());
	CHECK(codeOf(h->invoke("reset").reply()) == ActionRefusal::Code::unknownNode);
}

} // namespace actions_test
