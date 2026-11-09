// The node lifecycle's two pure machines and the aggregate, by table (docs/design/node-lifecycle.md sections 4.1 to
// 4.4 and 11.1; node-framework.md 6.2 and 6.3): every event in every block state and every configuration state, the
// derivation of phase, state, wanted and cause, every operation in every phase, and the review's cases.
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/node_machines.hpp"

using namespace ear::lifecycle;

namespace {

template <class... F> struct Overloaded : F... {
	using F::operator()...;
};
template <class... F> Overloaded(F...) -> Overloaded<F...>;

// ---- The block machine ----

BlockMachine reachBlock(BlockState state) {
	BlockMachine block;
	switch (state) {
	case BlockState::absent:
		break;
	case BlockState::waiting:
		block.on(BlockEvent::addedBehindOlder);
		break;
	case BlockState::made:
		block.on(BlockEvent::added);
		break;
	case BlockState::ready:
		block.on(BlockEvent::added);
		block.on(BlockEvent::ready);
		break;
	case BlockState::lost:
		block.on(BlockEvent::added);
		block.on(BlockEvent::lost, "no driver");
		break;
	case BlockState::gone:
		block.on(BlockEvent::added);
		block.on(BlockEvent::removed);
		break;
	}
	REQUIRE(block.state() == state);
	return block;
}

struct BlockCell {
	BlockState next;
	std::vector<BlockAction> actions;
};

// ---- The configuration machine ----

/// Where a configuration can be, with the facts that change what an event does.
enum class At {
	none,
	readying,
	prepared,
	arming,
	armingStopAsked,
	driving,
	drivingUnfaded,
	fading,
	silencing,
	tearing
};

std::string_view atName(At at) {
	switch (at) {
	case At::none:
		return "none";
	case At::readying:
		return "readying";
	case At::prepared:
		return "prepared";
	case At::arming:
		return "arming";
	case At::armingStopAsked:
		return "arming, stop asked";
	case At::driving:
		return "driving";
	case At::drivingUnfaded:
		return "driving, no output faded";
	case At::fading:
		return "fading";
	case At::silencing:
		return "silencing";
	case At::tearing:
		return "tearing";
	}
	return "?";
}

ConfigurationMachine reachConfig(At at) {
	ConfigurationMachine m;
	if (at == At::none) return m;
	m.on({ConfigEvent::configure, 0, at != At::drivingUnfaded});
	if (at == At::readying) return m;
	m.on({ConfigEvent::resourcesReady});
	if (at == At::prepared) return m;
	m.on({ConfigEvent::drive});
	if (at == At::arming) return m;
	if (at == At::armingStopAsked) {
		m.on({ConfigEvent::stopAsked});
		return m;
	}
	m.on({ConfigEvent::driven, m.barrier()});
	if (at == At::driving || at == At::drivingUnfaded) return m;
	m.on({ConfigEvent::stopAsked});
	if (at == At::fading) return m;
	m.on({ConfigEvent::faded, m.barrier()});
	if (at == At::silencing) return m;
	m.on({ConfigEvent::silenced, m.barrier()});
	return m;
}

ConfigState stateAt(At at) {
	switch (at) {
	case At::none:
		return ConfigState::none;
	case At::readying:
		return ConfigState::readying;
	case At::prepared:
		return ConfigState::prepared;
	case At::arming:
	case At::armingStopAsked:
		return ConfigState::arming;
	case At::driving:
	case At::drivingUnfaded:
		return ConfigState::driving;
	case At::fading:
		return ConfigState::fading;
	case At::silencing:
		return ConfigState::silencing;
	case At::tearing:
		return ConfigState::tearing;
	}
	return ConfigState::none;
}

using K = ConfigAction::Kind;

/// A cell: the next state (none: unchanged) and the actions' kinds; drive, fade and silence carry the next number.
struct ConfigCell {
	std::optional<ConfigState> next;
	std::vector<K> actions;
};
const ConfigCell same{std::nullopt, {}};

bool posted(K kind) { return kind == K::drive || kind == K::fade || kind == K::silence; }

// ---- The aggregate ----

/// A node and everything it told and asked, with the completions NodeHost would feed it.
struct Probe {
	NodeAggregate node;
	std::vector<Health> told;
	std::vector<K> asked;
	std::vector<BlockAction> blockAsked;
	bool gone = false;
	std::uint64_t version = 1;

	void take(const Actions& actions) {
		for (const NodeAction& action : actions)
			std::visit(Overloaded{[&](BlockAction a) { blockAsked.push_back(a); },
			                      [&](const ConfigAction& a) { asked.push_back(a.kind); },
			                      [&](const Tell& t) { told.push_back(t.health); }, [&](Gone) { gone = true; }},
			           action);
	}
	std::optional<LifecycleReply> take(Operated operated) {
		take(operated.actions);
		return operated.reply;
	}
	void clear() {
		told.clear();
		asked.clear();
		blockAsked.clear();
	}
	bool did(K kind) const {
		for (K k : asked)
			if (k == kind) return true;
		return false;
	}
	bool didBlock(BlockAction kind) const {
		for (BlockAction a : blockAsked)
			if (a == kind) return true;
		return false;
	}
	std::vector<NodePhase> phases() const {
		std::vector<NodePhase> out;
		for (const Health& h : told) out.push_back(h.phase);
		return out;
	}
	Health health() const { return node.health(); }
	NodePhase phase() const { return node.health().phase; }
	/// One completion NodeHost would get next, if any: true when one was fed.
	bool step() {
		const auto n = node.configuration().barrier();
		switch (node.configuration().state()) {
		case ConfigState::readying:
			take(node.resourcesReady());
			return true;
		case ConfigState::arming:
			take(node.driven(n));
			return true;
		case ConfigState::fading:
			take(node.faded(n));
			return true;
		case ConfigState::silencing:
			take(node.silenced(n));
			return true;
		case ConfigState::tearing:
			take(node.closed());
			return true;
		default:
			return false;
		}
	}
	void settle() {
		for (int i = 0; i < 64 && step(); ++i) {
		}
		REQUIRE_FALSE(step());
	}
	void add(AddFacts facts = {}) {
		facts.queued.version = version;
		take(node.add(facts));
	}
	void change(std::vector<std::string> keys = {"path"}) { take(node.change({++version, true}, std::move(keys))); }
};

std::vector<std::string_view> names(const std::vector<NodePhase>& phases) {
	std::vector<std::string_view> out;
	for (NodePhase p : phases) out.push_back(phaseName(p));
	return out;
}

/// Where 4.4's rows start.
enum class Spot {
	added,
	configuring,
	startingBlock, // prepared, the block not ready
	startingDrive, // a drive in flight
	running,
	stoppingStop,
	stoppingRestart,
	stoppingDisable,
	stoppingRemove,
	stopped,
	disabled,
	failed,
	failedLost,
};

std::string_view spotName(Spot spot) {
	switch (spot) {
	case Spot::added:
		return "added";
	case Spot::configuring:
		return "configuring";
	case Spot::startingBlock:
		return "starting, block not ready";
	case Spot::startingDrive:
		return "starting, drive in flight";
	case Spot::running:
		return "running";
	case Spot::stoppingStop:
		return "stopping (stop)";
	case Spot::stoppingRestart:
		return "stopping (restarting)";
	case Spot::stoppingDisable:
		return "stopping (disabling)";
	case Spot::stoppingRemove:
		return "stopping (removing)";
	case Spot::stopped:
		return "stopped";
	case Spot::disabled:
		return "disabled";
	case Spot::failed:
		return "failed, block kept";
	case Spot::failedLost:
		return "failed, block lost";
	}
	return "?";
}

Probe running() {
	Probe p;
	p.add();
	p.take(p.node.blockReady());
	p.settle();
	REQUIRE(p.phase() == NodePhase::running);
	return p;
}

Probe reach(Spot spot) {
	Probe p;
	switch (spot) {
	case Spot::added:
		p.add({.olderBlock = true});
		break;
	case Spot::configuring:
		p.add();
		p.take(p.node.blockReady());
		break;
	case Spot::startingBlock:
		p.add();
		p.take(p.node.resourcesReady());
		break;
	case Spot::startingDrive:
		p.add();
		p.take(p.node.blockReady());
		p.take(p.node.resourcesReady());
		REQUIRE(p.node.configuration().state() == ConfigState::arming);
		break;
	case Spot::running:
		p = running();
		break;
	case Spot::stoppingStop:
		p = running();
		p.take(p.node.stop());
		break;
	case Spot::stoppingRestart:
		p = running();
		p.take(p.node.restart());
		break;
	case Spot::stoppingDisable:
		p = running();
		p.take(p.node.disable());
		break;
	case Spot::stoppingRemove:
		p = running();
		p.take(p.node.remove());
		break;
	case Spot::stopped:
		p = running();
		p.take(p.node.stop());
		p.settle();
		break;
	case Spot::disabled:
		p.add({.disabled = true});
		break;
	case Spot::failed:
		p = running();
		p.take(p.node.fault(Fact::nodeLost, "it died"));
		p.settle();
		break;
	case Spot::failedLost:
		p = running();
		p.take(p.node.blockLost("no driver"));
		p.settle();
		break;
	}
	p.clear();
	return p;
}

enum class Op { change, stop, start, restart, disable, enable, remove, blockLost, fault };

std::string_view opName(Op op) {
	switch (op) {
	case Op::change:
		return "restart-only change";
	case Op::stop:
		return "stop";
	case Op::start:
		return "start";
	case Op::restart:
		return "restart";
	case Op::disable:
		return "disable";
	case Op::enable:
		return "enable";
	case Op::remove:
		return "removal";
	case Op::blockLost:
		return "block lost";
	case Op::fault:
		return "fault";
	}
	return "?";
}

std::optional<LifecycleReply> apply(Probe& p, Op op) {
	switch (op) {
	case Op::change:
		p.change();
		return std::nullopt;
	case Op::stop:
		return p.take(p.node.stop());
	case Op::start:
		return p.take(p.node.start());
	case Op::restart:
		return p.take(p.node.restart());
	case Op::disable:
		p.take(p.node.disable());
		return std::nullopt;
	case Op::enable:
		p.take(p.node.enable());
		return std::nullopt;
	case Op::remove:
		p.take(p.node.remove());
		return std::nullopt;
	case Op::blockLost:
		p.take(p.node.blockLost("unplugged"));
		return std::nullopt;
	case Op::fault:
		p.take(p.node.fault(Fact::nodeLost, "it died"));
		return std::nullopt;
	}
	return std::nullopt;
}

LifecycleReply ok(LifecycleApplied applied) { return applied; }
LifecycleReply no(LifecycleRefusal::Code code, LifecycleRefusal::Reason reason = LifecycleRefusal::Reason::none) {
	return std::unexpected(LifecycleRefusal{code, reason});
}

std::string replyName(const std::optional<LifecycleReply>& reply) {
	if (!reply) return "none";
	if (*reply) return std::string(appliedName(**reply));
	return "refused " + std::to_string(static_cast<int>(reply->error().code)) + "/" +
	       std::to_string(static_cast<int>(reply->error().reason));
}

} // namespace

TEST_CASE("Every event in every block state moves the block machine as its table says", "[lifecycle][machines]") {
	using enum BlockState;
	using A = BlockAction;
	const std::vector<BlockEvent> events{BlockEvent::addedWedged,    BlockEvent::added, BlockEvent::addedBehindOlder,
	                                     BlockEvent::olderDestroyed, BlockEvent::ready, BlockEvent::lost,
	                                     BlockEvent::removed};
	// Columns as events above; an event a state does not take leaves it with no action.
	const std::vector<std::pair<BlockState, std::vector<BlockCell>>> table{
		{absent,
	     {{lost, {}}, {made, {A::make}}, {waiting, {}}, {absent, {}}, {absent, {}}, {absent, {}}, {absent, {}}}},
		{waiting,
	     {{waiting, {}}, {waiting, {}}, {waiting, {}}, {made, {A::make}}, {waiting, {}}, {waiting, {}}, {gone, {}}}},
		{made, {{made, {}}, {made, {}}, {made, {}}, {made, {}}, {ready, {}}, {lost, {}}, {gone, {A::destroy}}}},
		{ready, {{ready, {}}, {ready, {}}, {ready, {}}, {ready, {}}, {ready, {}}, {lost, {}}, {gone, {A::destroy}}}},
		{lost, {{lost, {}}, {lost, {}}, {lost, {}}, {lost, {}}, {lost, {}}, {lost, {}}, {gone, {A::destroy}}}},
		{gone, {{gone, {}}, {gone, {}}, {gone, {}}, {gone, {}}, {gone, {}}, {gone, {}}, {gone, {}}}},
	};
	REQUIRE(table.size() == 6);
	for (const auto& [from, cells] : table) {
		REQUIRE(cells.size() == events.size());
		for (std::size_t i = 0; i < events.size(); ++i) {
			INFO("block " << blockStateName(from) << ", event " << blockEventName(events[i]));
			BlockMachine block = reachBlock(from);
			const std::string detailBefore = block.lossDetail();
			const auto actions = block.on(events[i], "card gone");
			CHECK(blockStateName(block.state()) == blockStateName(cells[i].next));
			CHECK(actions == cells[i].actions);
			// A loss keeps its detail: the wedge's own, or the event's; nothing else changes it.
			if (from == absent && events[i] == BlockEvent::addedWedged) CHECK(block.lossDetail() == wedgedDetail);
			else if (cells[i].next == lost && from != lost) CHECK(block.lossDetail() == "card gone");
			else CHECK(block.lossDetail() == detailBefore);
		}
	}
}

TEST_CASE("Every event in every configuration state moves the configuration machine as its table says",
          "[lifecycle][machines]") {
	using enum ConfigState;
	const std::vector<ConfigEvent> events{
		ConfigEvent::configure,     ConfigEvent::resourcesReady, ConfigEvent::stopAsked,       ConfigEvent::startAsked,
		ConfigEvent::teardownAsked, ConfigEvent::rest,           ConfigEvent::drive,           ConfigEvent::driven,
		ConfigEvent::faded,         ConfigEvent::fadeBound,      ConfigEvent::silencedForStop, ConfigEvent::silenced,
		ConfigEvent::barrierBound,  ConfigEvent::closed,         ConfigEvent::closeBound,      ConfigEvent::blockLost,
		ConfigEvent::startThrew};
	// Columns as events above.
	const ConfigCell toTearing{tearing, {K::teardown}};
	const ConfigCell toTearingReleased{tearing, {K::release, K::teardown}};
	const ConfigCell lostToSilence{silencing, {K::silence}};
	// A drive's invoke not run in 5 s is the same fact about the loop as a silence's (revision 2.7).
	const ConfigCell leakedOverdue{none, {K::barrierOverdue, K::markWedged, K::leak}};
	const std::vector<std::pair<At, std::vector<ConfigCell>>> table{
		{At::none,
	     {{readying, {K::configure}},
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same}},
		{At::readying,
	     {same,
	      {prepared, {K::runRecipe, K::prepare}},
	      same,
	      same,
	      toTearing,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      toTearing,
	      same}},
		{At::prepared,
	     {same,
	      same,
	      same,
	      same,
	      toTearingReleased,
	      same,
	      {arming, {K::drive}},
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      toTearingReleased,
	      same}},
		{At::arming,
	     {same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      {driving, {K::start}},
	      same,
	      same,
	      same,
	      same,
	      leakedOverdue,
	      same,
	      same,
	      lostToSilence,
	      same}},
		{At::armingStopAsked,
	     {same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      {fading, {K::fade}},
	      same,
	      same,
	      same,
	      same,
	      leakedOverdue,
	      same,
	      same,
	      lostToSilence,
	      same}},
		{At::driving,
	     {same,
	      same,
	      {fading, {K::stop, K::fade}},
	      same,
	      {fading, {K::stop, K::fade}},
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      lostToSilence,
	      same}},
		{At::drivingUnfaded,
	     {same,
	      same,
	      {silencing, {K::stop, K::silence}},
	      same,
	      {silencing, {K::stop, K::silence}},
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      lostToSilence,
	      same}},
		{At::fading,
	     {same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      {silencing, {K::silence}},
	      {silencing, {K::fadeUnacknowledged, K::silence}},
	      same,
	      same,
	      same,
	      same,
	      same,
	      lostToSilence,
	      same}},
		{At::silencing,
	     {same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      {prepared, {K::release, K::prepare}},
	      toTearingReleased,
	      {none, {K::barrierOverdue, K::markWedged, K::leak}},
	      same,
	      same,
	      same,
	      same}},
		{At::tearing,
	     {same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      same,
	      {none, {K::free}},
	      {none, {K::closeOverdue, K::leak}},
	      same,
	      same}},
	};
	REQUIRE(table.size() == 10);
	for (const auto& [from, cells] : table) {
		REQUIRE(cells.size() == events.size());
		for (std::size_t i = 0; i < events.size(); ++i) {
			INFO("configuration " << atName(from) << ", event " << configEventName(events[i]));
			ConfigurationMachine m = reachConfig(from);
			const auto before = m.barrier();
			const auto actions = m.on({events[i], before});
			const ConfigState next = cells[i].next.value_or(stateAt(from));
			CHECK(configStateName(m.state()) == configStateName(next));
			std::vector<K> kinds;
			for (const ConfigAction& a : actions) {
				kinds.push_back(a.kind);
				// Every request posted raises the barrier number, and carries it.
				if (posted(a.kind)) CHECK(a.barrier == before + 1);
				else CHECK(a.barrier == 0);
			}
			CHECK(kinds == cells[i].actions);
		}
	}
}

TEST_CASE("A completion with an older barrier number changes nothing", "[lifecycle][machines]") {
	for (At at : {At::arming, At::fading, At::silencing}) {
		INFO(atName(at));
		ConfigurationMachine m = reachConfig(at);
		const ConfigState state = m.state();
		const auto stale = m.barrier() - 1;
		for (ConfigEvent e :
		     {ConfigEvent::driven, ConfigEvent::faded, ConfigEvent::silencedForStop, ConfigEvent::silenced}) {
			CHECK(m.on({e, stale}).empty());
			CHECK(m.state() == state);
		}
	}
}

TEST_CASE("The configuration's facts: started, driven, rested and stopAsked", "[lifecycle][machines]") {
	SECTION("a drive sets driven when posted, started and rested once driven, and a silence clears driven") {
		ConfigurationMachine m = reachConfig(At::prepared);
		CHECK_FALSE(m.driven());
		m.on({ConfigEvent::drive});
		CHECK(m.driven());
		CHECK_FALSE(m.started());
		CHECK_FALSE(m.rested());
		m.on({ConfigEvent::driven, m.barrier()});
		CHECK(m.started());
		CHECK(m.rested());
		m.on({ConfigEvent::stopAsked});
		CHECK_FALSE(m.started()); // stop() was called
		m.on({ConfigEvent::faded, m.barrier()});
		m.on({ConfigEvent::silencedForStop, m.barrier()});
		CHECK_FALSE(m.driven());
		CHECK(m.rested());
	}
	SECTION("a rest sets rested once and moves nothing") {
		ConfigurationMachine m = reachConfig(At::prepared);
		CHECK(m.on({ConfigEvent::rest}).empty());
		CHECK(m.rested());
		CHECK(m.state() == ConfigState::prepared);
	}
	SECTION("a start() that threw never set started, so no stop() pairs with it, and the node has not rested") {
		ConfigurationMachine m = reachConfig(At::driving);
		m.on({ConfigEvent::startThrew});
		CHECK_FALSE(m.started());
		CHECK_FALSE(m.rested());
		const auto actions = m.on({ConfigEvent::teardownAsked});
		REQUIRE(actions.size() == 1);
		CHECK(actions[0].kind == K::fade);
	}
	SECTION("a start in arming clears a stop asked, so the drive's completion starts the code") {
		ConfigurationMachine m = reachConfig(At::armingStopAsked);
		CHECK(m.stopAsked());
		m.on({ConfigEvent::startAsked});
		CHECK_FALSE(m.stopAsked());
		const auto actions = m.on({ConfigEvent::driven, m.barrier()});
		REQUIRE(actions.size() == 1);
		CHECK(actions[0].kind == K::start);
	}
	SECTION("a block lost while driving clears started without stop()") {
		ConfigurationMachine m = reachConfig(At::driving);
		m.on({ConfigEvent::blockLost});
		CHECK_FALSE(m.started());
		CHECK(m.state() == ConfigState::silencing);
	}
	SECTION("configure starts every fact afresh") {
		ConfigurationMachine m = reachConfig(At::tearing);
		m.on({ConfigEvent::closed});
		m.on({ConfigEvent::configure, 0, false});
		CHECK_FALSE(m.started());
		CHECK_FALSE(m.driven());
		CHECK_FALSE(m.rested());
		CHECK_FALSE(m.stopAsked());
		CHECK_FALSE(m.fades());
	}
}

TEST_CASE("The failure code is chosen by the fact: engine-refused always for the engine, start-failed or lost by "
          "rested for the node",
          "[lifecycle][machines]") {
	using enum FailureCode;
	const std::vector<std::tuple<Fact, FailureCode, FailureCode>> table{
		{Fact::engine, engineRefused, engineRefused},
		{Fact::configureFailed, startFailed, startFailed},
		{Fact::consumerLost, startFailed, startFailed},
		{Fact::recipeFailed, startFailed, startFailed},
		{Fact::nodeLost, startFailed, lost},
		{Fact::hookThrew, startFailed, lost},
	};
	for (const auto& [fact, before, after] : table) {
		INFO("fact " << static_cast<int>(fact));
		CHECK(failureCodeFor(fact, false) == before);
		CHECK(failureCodeFor(fact, true) == after);
	}
	SECTION("through the aggregate") {
		Probe p;
		p.add();
		p.take(p.node.fault(Fact::configureFailed, "bad path"));
		CHECK(p.health().reason == Failure{startFailed, "bad path"});
		Probe r = running();
		r.take(r.node.fault(Fact::hookThrew, "process() threw"));
		CHECK(r.health().reason == Failure{lost, "process() threw"});
		Probe e = running();
		e.take(e.node.fault(Fact::engine, "card gone"));
		CHECK(e.health().reason == Failure{engineRefused, "card gone"});
	}
}

TEST_CASE("The phase, state, wanted and cause derived for every combination of the machines and facts",
          "[lifecycle][machines]") {
	using P = NodePhase;
	using CK = NodeCause::Kind;
	const Failure failure{FailureCode::startFailed, "bad"};
	const NodeCause action{CK::action, std::nullopt, {}};
	const NodeCause enabling{CK::enabling, std::nullopt, {}};
	const NodeCause settings{CK::settings, std::nullopt, {"port"}};
	struct Row {
		std::string what;
		DeriveFacts facts;
		P phase;
		std::optional<NodeCause> cause;
	};
	const auto restarting = [](const NodeCause& why) { return NodeCause{CK::restarting, why.kind, why.keys}; };
	// 4.3's rows in order, each with facts a later row would also match, so first match wins.
	const std::vector<Row> rows{
		{"removing beats a failure",
	     {.config = ConfigState::fading, .intent = Intent::remove, .failure = failure},
	     P::stopping,
	     NodeCause{CK::removing, std::nullopt, {}}},
		{"a predecessor still tearing down",
	     {.block = BlockState::waiting, .why = settings, .predecessor = true},
	     P::stopping,
	     restarting(settings)},
		{"a failure, its teardown invisible",
	     {.config = ConfigState::tearing, .intent = Intent::fail, .failure = failure},
	     P::failed,
	     std::nullopt},
		{"a block lost, not disabled", {.block = BlockState::lost, .blockLossDetail = "gone"}, P::failed, std::nullopt},
		{"a block lost while disabled", {.block = BlockState::lost, .disabled = true}, P::disabled, std::nullopt},
		{"disabled with no configuration", {.disabled = true, .stopHeld = false}, P::disabled, std::nullopt},
		{"disabled while its teardown runs",
	     {.config = ConfigState::tearing, .disabled = true, .intent = Intent::disable},
	     P::stopping,
	     NodeCause{CK::disabling, std::nullopt, {}}},
		{"a stop's fade",
	     {.config = ConfigState::fading, .intent = Intent::stop},
	     P::stopping,
	     NodeCause{CK::stop, std::nullopt, {}}},
		{"a restart's silence",
	     {.config = ConfigState::silencing, .intent = Intent::restart, .why = action},
	     P::stopping,
	     restarting(action)},
		{"a restart's teardown ended, configure follows",
	     {.config = ConfigState::none, .intent = Intent::restart, .why = settings},
	     P::stopping,
	     restarting(settings)},
		{"a restart while a drive is in flight",
	     {.config = ConfigState::arming, .stopAsked = true, .intent = Intent::restart, .why = action},
	     P::stopping,
	     restarting(action)},
		{"a stop while a drive is in flight",
	     {.config = ConfigState::arming, .stopAsked = true, .stopHeld = true, .intent = Intent::stop},
	     P::stopping,
	     NodeCause{CK::stop, std::nullopt, {}}},
		{"waiting for an older node", {.block = BlockState::waiting}, P::added, std::nullopt},
		{"configuring from empty", {.config = ConfigState::readying}, P::configuring, std::nullopt},
		{"configuring after an enable", {.config = ConfigState::readying, .why = enabling}, P::configuring, enabling},
		{"prepared and held stopped", {.config = ConfigState::prepared, .stopHeld = true}, P::stopped, std::nullopt},
		{"prepared, not yet told", {.config = ConfigState::prepared}, P::configured, std::nullopt},
		{"prepared, told", {.config = ConfigState::prepared, .configuredTold = true}, P::starting, std::nullopt},
		{"prepared after resting", {.config = ConfigState::prepared, .rested = true}, P::starting, std::nullopt},
		{"arming", {.block = BlockState::ready, .config = ConfigState::arming}, P::starting, std::nullopt},
		{"driving", {.block = BlockState::ready, .config = ConfigState::driving}, P::running, std::nullopt},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		const Health h = derive(row.facts);
		CHECK(phaseName(h.phase) == phaseName(row.phase));
		CHECK(h.cause == row.cause);
	}
	CHECK(derive({.block = BlockState::lost, .blockLossDetail = "gone"}).reason ==
	      Failure{FailureCode::engineRefused, "gone"});
	CHECK(removed({.block = BlockState::gone, .intent = Intent::remove}));
	CHECK_FALSE(removed({.block = BlockState::gone, .config = ConfigState::tearing, .intent = Intent::remove}));

	SECTION("every combination keeps the members consistent") {
		const std::vector<BlockState> blocks{BlockState::waiting, BlockState::made, BlockState::ready, BlockState::lost,
		                                     BlockState::gone};
		const std::vector<ConfigState> configs{ConfigState::none,      ConfigState::readying, ConfigState::prepared,
		                                       ConfigState::arming,    ConfigState::driving,  ConfigState::fading,
		                                       ConfigState::silencing, ConfigState::tearing};
		const std::vector<Intent> intents{Intent::none, Intent::stop,    Intent::restart,
		                                  Intent::fail, Intent::disable, Intent::remove};
		std::size_t combinations = 0;
		for (BlockState block : blocks)
			for (ConfigState config : configs)
				for (Intent intent : intents)
					for (int bits = 0; bits < 64; ++bits) {
						DeriveFacts f{.block = block, .blockLossDetail = "gone", .config = config};
						f.intent = intent;
						f.disabled = bits & 1;
						f.stopHeld = bits & 2;
						if (bits & 4) f.failure = failure;
						f.predecessor = bits & 8;
						f.configuredTold = bits & 16;
						f.rested = bits & 32;
						f.stopAsked = config == ConfigState::arming && (bits & 16);
						const Health h = derive(f);
						++combinations;
						INFO("block " << blockStateName(block) << ", configuration " << configStateName(config)
						              << ", intent " << intentName(intent) << ", bits " << bits);
						CHECK(h.state == stateOf(h.phase));
						CHECK(h.wanted == (f.disabled   ? Wanted::disabled
						                   : f.stopHeld ? Wanted::stopped
						                                : Wanted::running));
						CHECK(h.blockLost == (block == BlockState::lost));
						CHECK(h.reason.has_value() == (h.phase == NodePhase::failed));
						// fail is never in flight without a failure or a lost block, both of which derive failed.
						if (h.phase == NodePhase::stopping && intent != Intent::none && intent != Intent::fail)
							CHECK(h.cause.has_value());
						if (h.cause) CHECK((h.phase == NodePhase::stopping || h.phase == NodePhase::configuring));
						if (intent == Intent::remove) CHECK(h.phase == NodePhase::stopping);
						if (h.phase == NodePhase::running) CHECK(config == ConfigState::driving);
					}
		CHECK(combinations == 5 * 8 * 6 * 64);
	}
}

TEST_CASE("Phases, states, causes and failure codes have their names", "[lifecycle][machines]") {
	using P = NodePhase;
	const std::vector<std::tuple<P, std::string_view, std::string_view>> phases{
		{P::added, "added", "starting"},           {P::configuring, "configuring", "starting"},
		{P::configured, "configured", "starting"}, {P::starting, "starting", "starting"},
		{P::running, "running", "running"},        {P::stopping, "stopping", "stopping"},
		{P::stopped, "stopped", "stopped"},        {P::disabled, "disabled", "disabled"},
		{P::failed, "failed", "failed"},
	};
	for (const auto& [phase, name, state] : phases) {
		CHECK(phaseName(phase) == name);
		CHECK(stateName(stateOf(phase)) == state);
	}
	using CK = NodeCause::Kind;
	CHECK(causeName(CK::stop) == "stop");
	CHECK(causeName(CK::removing) == "removing");
	CHECK(causeName(CK::disabling) == "disabling");
	CHECK(causeName(CK::restarting) == "restarting");
	CHECK(causeName(CK::settings) == "settings");
	CHECK(causeName(CK::action) == "action");
	CHECK(causeName(CK::retry) == "retry");
	CHECK(causeName(CK::enabling) == "enabling");
	CHECK(failureCodeName(FailureCode::startFailed) == "start-failed");
	CHECK(failureCodeName(FailureCode::engineRefused) == "engine-refused");
	CHECK(failureCodeName(FailureCode::lost) == "lost");
	CHECK(wantedName(Wanted::running) == "running");
	CHECK(wantedName(Wanted::stopped) == "stopped");
	CHECK(wantedName(Wanted::disabled) == "disabled");
	CHECK(appliedName(LifecycleApplied::retry) == "retry");
	CHECK(appliedName(LifecycleApplied::unchanged) == "unchanged");
}

TEST_CASE("Every operation in every phase answers and moves as section 4.4 says", "[lifecycle][machines]") {
	using P = NodePhase;
	using CK = NodeCause::Kind;
	using A = LifecycleApplied;
	using C = LifecycleRefusal::Code;
	using R = LifecycleRefusal::Reason;
	struct Row {
		Spot spot;
		Op op;
		std::optional<LifecycleReply> reply;
		P phase;                  // at once
		std::optional<CK> cause;  // at once
		std::optional<P> settled; // once every completion arrived; none: removed
	};
	const std::optional<LifecycleReply> none;
	const std::optional<P> removedAfter;
	// One row per cell of 4.4. A block lost while waiting for an older node cannot arrive (no block exists): the
	// added row's block-lost cell checks that it changes nothing. "configured" lasts no input (row 10 is told and
	// derived again at once), so configuring's row stands for both.
	const std::vector<Row> rows{
		{Spot::added, Op::change, none, P::added, {}, P::added},
		{Spot::added, Op::stop, ok(A::stop), P::added, {}, P::added},
		{Spot::added, Op::start, ok(A::unchanged), P::added, {}, P::added},
		{Spot::added, Op::restart, no(C::notRestartable, R::notStarted), P::added, {}, P::added},
		{Spot::added, Op::disable, none, P::disabled, {}, P::disabled},
		{Spot::added, Op::enable, none, P::added, {}, P::added},
		{Spot::added, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::added, Op::blockLost, none, P::added, {}, P::added},
		{Spot::added, Op::fault, none, P::added, {}, P::added},

		{Spot::configuring, Op::change, none, P::stopping, CK::restarting, P::running},
		{Spot::configuring, Op::stop, ok(A::stop), P::configuring, {}, P::stopped},
		{Spot::configuring, Op::start, ok(A::unchanged), P::configuring, {}, P::running},
		{Spot::configuring, Op::restart, ok(A::restart), P::stopping, CK::restarting, P::running},
		{Spot::configuring, Op::disable, none, P::stopping, CK::disabling, P::disabled},
		{Spot::configuring, Op::enable, none, P::configuring, {}, P::running},
		{Spot::configuring, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::configuring, Op::blockLost, none, P::failed, {}, P::failed},
		{Spot::configuring, Op::fault, none, P::failed, {}, P::failed},

		{Spot::startingBlock, Op::change, none, P::stopping, CK::restarting, P::starting},
		{Spot::startingBlock, Op::stop, ok(A::stop), P::stopped, {}, P::stopped},
		{Spot::startingBlock, Op::start, ok(A::unchanged), P::starting, {}, P::starting},
		{Spot::startingBlock, Op::restart, ok(A::restart), P::stopping, CK::restarting, P::starting},
		{Spot::startingBlock, Op::disable, none, P::stopping, CK::disabling, P::disabled},
		{Spot::startingBlock, Op::enable, none, P::starting, {}, P::starting},
		{Spot::startingBlock, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::startingBlock, Op::blockLost, none, P::failed, {}, P::failed},
		{Spot::startingBlock, Op::fault, none, P::failed, {}, P::failed},

		{Spot::startingDrive, Op::change, none, P::stopping, CK::restarting, P::running},
		{Spot::startingDrive, Op::stop, ok(A::stop), P::stopping, CK::stop, P::stopped},
		{Spot::startingDrive, Op::start, ok(A::unchanged), P::starting, {}, P::running},
		{Spot::startingDrive, Op::restart, ok(A::restart), P::stopping, CK::restarting, P::running},
		{Spot::startingDrive, Op::disable, none, P::stopping, CK::disabling, P::disabled},
		{Spot::startingDrive, Op::enable, none, P::starting, {}, P::running},
		{Spot::startingDrive, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::startingDrive, Op::blockLost, none, P::failed, {}, P::failed},
		{Spot::startingDrive, Op::fault, none, P::failed, {}, P::failed},

		{Spot::running, Op::change, none, P::stopping, CK::restarting, P::running},
		{Spot::running, Op::stop, ok(A::stop), P::stopping, CK::stop, P::stopped},
		{Spot::running, Op::start, ok(A::unchanged), P::running, {}, P::running},
		{Spot::running, Op::restart, ok(A::restart), P::stopping, CK::restarting, P::running},
		{Spot::running, Op::disable, none, P::stopping, CK::disabling, P::disabled},
		{Spot::running, Op::enable, none, P::running, {}, P::running},
		{Spot::running, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::running, Op::blockLost, none, P::failed, {}, P::failed},
		{Spot::running, Op::fault, none, P::failed, {}, P::failed},

		{Spot::stoppingStop, Op::change, none, P::stopping, CK::stop, P::stopped},
		{Spot::stoppingStop, Op::stop, ok(A::unchanged), P::stopping, CK::stop, P::stopped},
		{Spot::stoppingStop, Op::start, ok(A::start), P::stopping, CK::stop, P::running},
		{Spot::stoppingStop, Op::restart, ok(A::restart), P::stopping, CK::restarting, P::stopped},
		{Spot::stoppingStop, Op::disable, none, P::stopping, CK::disabling, P::disabled},
		{Spot::stoppingStop, Op::enable, none, P::stopping, CK::stop, P::stopped},
		{Spot::stoppingStop, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::stoppingStop, Op::blockLost, none, P::failed, {}, P::failed},
		{Spot::stoppingStop, Op::fault, none, P::stopping, CK::stop, P::failed},

		{Spot::stoppingRestart, Op::change, none, P::stopping, CK::restarting, P::running},
		{Spot::stoppingRestart, Op::stop, ok(A::stop), P::stopping, CK::restarting, P::stopped},
		{Spot::stoppingRestart, Op::start, ok(A::unchanged), P::stopping, CK::restarting, P::running},
		{Spot::stoppingRestart, Op::restart, ok(A::unchanged), P::stopping, CK::restarting, P::running},
		{Spot::stoppingRestart, Op::disable, none, P::stopping, CK::disabling, P::disabled},
		{Spot::stoppingRestart, Op::enable, none, P::stopping, CK::restarting, P::running},
		{Spot::stoppingRestart, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::stoppingRestart, Op::blockLost, none, P::failed, {}, P::failed},
		{Spot::stoppingRestart, Op::fault, none, P::stopping, CK::restarting, P::running},

		{Spot::stoppingDisable, Op::change, none, P::stopping, CK::disabling, P::disabled},
		{Spot::stoppingDisable, Op::stop, no(C::nodeDisabled), P::stopping, CK::disabling, P::disabled},
		{Spot::stoppingDisable, Op::start, no(C::nodeDisabled), P::stopping, CK::disabling, P::disabled},
		{Spot::stoppingDisable, Op::restart, no(C::nodeDisabled), P::stopping, CK::disabling, P::disabled},
		{Spot::stoppingDisable, Op::disable, none, P::stopping, CK::disabling, P::disabled},
		{Spot::stoppingDisable, Op::enable, none, P::stopping, CK::restarting, P::running},
		{Spot::stoppingDisable, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::stoppingDisable, Op::blockLost, none, P::stopping, CK::disabling, P::disabled},
		{Spot::stoppingDisable, Op::fault, none, P::stopping, CK::disabling, P::disabled},

		{Spot::stoppingRemove, Op::change, none, P::stopping, CK::removing, removedAfter},
		{Spot::stoppingRemove, Op::stop, no(C::unknownNode), P::stopping, CK::removing, removedAfter},
		{Spot::stoppingRemove, Op::start, no(C::unknownNode), P::stopping, CK::removing, removedAfter},
		{Spot::stoppingRemove, Op::restart, no(C::unknownNode), P::stopping, CK::removing, removedAfter},
		{Spot::stoppingRemove, Op::disable, none, P::stopping, CK::removing, removedAfter},
		{Spot::stoppingRemove, Op::enable, none, P::stopping, CK::removing, removedAfter},
		{Spot::stoppingRemove, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::stoppingRemove, Op::blockLost, none, P::stopping, CK::removing, removedAfter},
		{Spot::stoppingRemove, Op::fault, none, P::stopping, CK::removing, removedAfter},

		{Spot::stopped, Op::change, none, P::stopping, CK::restarting, P::stopped},
		{Spot::stopped, Op::stop, ok(A::unchanged), P::stopped, {}, P::stopped},
		{Spot::stopped, Op::start, ok(A::start), P::starting, {}, P::running},
		{Spot::stopped, Op::restart, ok(A::restart), P::stopping, CK::restarting, P::stopped},
		{Spot::stopped, Op::disable, none, P::stopping, CK::disabling, P::disabled},
		{Spot::stopped, Op::enable, none, P::stopped, {}, P::stopped},
		{Spot::stopped, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::stopped, Op::blockLost, none, P::failed, {}, P::failed},
		{Spot::stopped, Op::fault, none, P::failed, {}, P::failed},

		{Spot::disabled, Op::change, none, P::disabled, {}, P::disabled},
		{Spot::disabled, Op::stop, no(C::nodeDisabled), P::disabled, {}, P::disabled},
		{Spot::disabled, Op::start, no(C::nodeDisabled), P::disabled, {}, P::disabled},
		{Spot::disabled, Op::restart, no(C::nodeDisabled), P::disabled, {}, P::disabled},
		{Spot::disabled, Op::disable, none, P::disabled, {}, P::disabled},
		{Spot::disabled, Op::enable, none, P::configuring, CK::enabling, P::starting},
		{Spot::disabled, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::disabled, Op::blockLost, none, P::disabled, {}, P::disabled},
		{Spot::disabled, Op::fault, none, P::disabled, {}, P::disabled},

		{Spot::failed, Op::change, none, P::configuring, CK::settings, P::running},
		{Spot::failed, Op::stop, ok(A::stop), P::failed, {}, P::failed},
		{Spot::failed, Op::start, ok(A::retry), P::configuring, CK::retry, P::running},
		{Spot::failed, Op::restart, ok(A::retry), P::configuring, CK::retry, P::running},
		{Spot::failed, Op::disable, none, P::disabled, {}, P::disabled},
		{Spot::failed, Op::enable, none, P::failed, {}, P::failed},
		{Spot::failed, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::failed, Op::blockLost, none, P::failed, {}, P::failed},
		{Spot::failed, Op::fault, none, P::failed, {}, P::failed},

		{Spot::failedLost, Op::change, none, P::failed, {}, P::failed},
		{Spot::failedLost, Op::stop, ok(A::stop), P::failed, {}, P::failed},
		{Spot::failedLost, Op::start, no(C::notRestartable, R::blockLost), P::failed, {}, P::failed},
		{Spot::failedLost, Op::restart, no(C::notRestartable, R::blockLost), P::failed, {}, P::failed},
		{Spot::failedLost, Op::disable, none, P::disabled, {}, P::disabled},
		{Spot::failedLost, Op::enable, none, P::failed, {}, P::failed},
		{Spot::failedLost, Op::remove, none, P::stopping, CK::removing, removedAfter},
		{Spot::failedLost, Op::blockLost, none, P::failed, {}, P::failed},
		{Spot::failedLost, Op::fault, none, P::failed, {}, P::failed},
	};
	CHECK(rows.size() == 13 * 9);
	for (const Row& row : rows) {
		INFO(spotName(row.spot) << ", " << opName(row.op));
		Probe p = reach(row.spot);
		const auto reply = apply(p, row.op);
		CHECK(replyName(reply) == replyName(row.reply));
		const bool removedAtOnce = p.node.removed();
		if (!removedAtOnce) {
			const Health h = p.health();
			CHECK(phaseName(h.phase) == phaseName(row.phase));
			const std::optional<CK> cause = h.cause ? std::optional{h.cause->kind} : std::nullopt;
			CHECK(cause == row.cause);
		} else {
			CHECK(p.gone);
		}
		p.settle();
		if (row.settled) {
			REQUIRE_FALSE(p.node.removed());
			CHECK(phaseName(p.phase()) == phaseName(*row.settled));
		} else {
			CHECK(p.node.removed());
			CHECK(p.gone);
		}
	}
}

TEST_CASE("4.4's hooks and barriers: stop() only after a start() that returned, a fade only for a driving code",
          "[lifecycle][machines]") {
	SECTION("running: a stop calls stop() and fades") {
		Probe p = reach(Spot::running);
		p.take(p.node.stop());
		CHECK(p.did(K::stop));
		CHECK(p.did(K::fade));
	}
	SECTION("running: a block lost calls no stop() and fades nothing") {
		Probe p = reach(Spot::running);
		p.take(p.node.blockLost("gone"));
		CHECK_FALSE(p.did(K::stop));
		CHECK_FALSE(p.did(K::fade));
		CHECK(p.did(K::silence));
	}
	SECTION("a stop in starting, with the drive in flight, ends stopped with no start() or stop()") {
		Probe p = reach(Spot::startingDrive);
		p.take(p.node.stop());
		p.settle();
		CHECK(p.phase() == NodePhase::stopped);
		CHECK_FALSE(p.did(K::start));
		CHECK_FALSE(p.did(K::stop));
		CHECK(p.did(K::fade));
	}
	SECTION("a fault in starting calls no stop()") {
		Probe p = reach(Spot::startingDrive);
		p.take(p.node.fault(Fact::engine, "card gone"));
		p.settle();
		CHECK_FALSE(p.did(K::stop));
		CHECK_FALSE(p.did(K::start));
	}
	SECTION("stopped: a disable, a restart or a removal posts no barrier") {
		for (Op op : {Op::disable, Op::restart, Op::change, Op::remove}) {
			INFO(opName(op));
			Probe p = reach(Spot::stopped);
			apply(p, op);
			p.settle();
			CHECK_FALSE(p.did(K::stop));
			CHECK_FALSE(p.did(K::fade));
			CHECK_FALSE(p.did(K::silence));
		}
	}
	SECTION("stopped: a removal destroys the block at once (D-L7)") {
		Probe p = reach(Spot::stopped);
		p.take(p.node.remove());
		CHECK(p.didBlock(BlockAction::destroy));
		CHECK(p.node.block().state() == BlockState::gone);
	}
	SECTION("running: a removal destroys the block only once silenced") {
		Probe p = reach(Spot::running);
		p.take(p.node.remove());
		CHECK_FALSE(p.didBlock(BlockAction::destroy));
		p.step(); // faded
		CHECK_FALSE(p.didBlock(BlockAction::destroy));
		p.step(); // silenced
		CHECK(p.didBlock(BlockAction::destroy));
		CHECK_FALSE(p.gone);
		p.step(); // closed
		CHECK(p.gone);
	}
	SECTION("stopped: a fault is lost, with no stop() (it ran at the stop)") {
		Probe p = reach(Spot::stopped);
		p.take(p.node.fault(Fact::nodeLost, "the library died"));
		CHECK(p.health().reason == Failure{FailureCode::lost, "the library died"});
		CHECK_FALSE(p.did(K::stop));
	}
}

TEST_CASE("A normal start tells added, configuring, configured, starting, running", "[lifecycle][machines]") {
	Probe p;
	p.add();
	CHECK(p.didBlock(BlockAction::make));
	CHECK(p.did(K::configure));
	p.take(p.node.blockReady());
	p.settle();
	CHECK(names(p.phases()) ==
	      std::vector<std::string_view>{"added", "configuring", "configured", "starting", "running"});
	CHECK(p.asked == std::vector<K>{K::configure, K::runRecipe, K::prepare, K::drive, K::start});
}

TEST_CASE("Every configure enters readying, so configuring is derived even with nothing pending",
          "[lifecycle][machines]") {
	Probe p;
	p.add();
	CHECK(p.node.configuration().state() == ConfigState::readying);
	CHECK(p.phase() == NodePhase::configuring);
	SECTION("and after a restart, with its cause") {
		Probe r = reach(Spot::stopped);
		r.take(r.node.restart());
		r.settle();
		// A restart of a stopped node: stopping (restarting), configuring (action), stopped; configured is skipped.
		CHECK(names(r.phases()) == std::vector<std::string_view>{"stopping", "configuring", "stopped"});
		REQUIRE(r.told.size() == 3);
		CHECK(r.told[0].cause == NodeCause{NodeCause::Kind::restarting, NodeCause::Kind::action, {}});
		CHECK(r.told[1].cause == NodeCause{NodeCause::Kind::action, std::nullopt, {}});
	}
}

TEST_CASE("A stop and a start tell stopping, stopped, starting, running, with no configured", "[lifecycle][machines]") {
	Probe p = reach(Spot::running);
	p.take(p.node.stop());
	p.settle();
	p.take(p.node.start());
	p.settle();
	CHECK(names(p.phases()) == std::vector<std::string_view>{"stopping", "stopped", "starting", "running"});
	CHECK(p.told[0].wanted == Wanted::stopped);
	CHECK(p.told[2].wanted == Wanted::running);
	// Every silence for a stop releases and prepares the code afresh (D-L33).
	CHECK(p.asked == std::vector<K>{K::stop, K::fade, K::silence, K::release, K::prepare, K::drive, K::start});
}

TEST_CASE("A node added disabled tells only disabled, and one waiting for an older node configures once it is gone",
          "[lifecycle][machines]") {
	Probe d;
	d.add({.disabled = true});
	CHECK(names(d.phases()) == std::vector<std::string_view>{"disabled"});
	CHECK(d.didBlock(BlockAction::make));
	CHECK_FALSE(d.did(K::configure));

	Probe w;
	w.add({.olderBlock = true});
	CHECK(names(w.phases()) == std::vector<std::string_view>{"added"});
	CHECK(w.node.waitingForOlder());
	CHECK(w.blockAsked.empty());
	w.take(w.node.olderDestroyed());
	CHECK(w.didBlock(BlockAction::make));
	CHECK(w.phase() == NodePhase::configuring);
}

TEST_CASE("The newest intent wins: remove over disable over fail over restart over stop, only while one is in "
          "flight",
          "[lifecycle][machines]") {
	Probe p = reach(Spot::running);
	p.take(p.node.stop());
	CHECK(p.node.intent() == Intent::stop);
	p.take(p.node.restart());
	CHECK(p.node.intent() == Intent::restart);
	p.take(p.node.disable());
	CHECK(p.node.intent() == Intent::disable);
	p.take(p.node.fault(Fact::nodeLost, "late")); // lower than disable, and dropped in a teardown anyway
	CHECK(p.node.intent() == Intent::disable);
	p.take(p.node.blockLost("gone")); // fail is lower than disable
	CHECK(p.node.intent() == Intent::disable);
	p.take(p.node.remove());
	CHECK(p.node.intent() == Intent::remove);
	p.settle();
	CHECK(p.gone);

	SECTION("a lower intent does not replace a higher one") {
		Probe q = reach(Spot::running);
		q.take(q.node.restart());
		q.take(q.node.stop()); // sets the stop, but the restart stays the intent
		CHECK(q.node.intent() == Intent::restart);
		CHECK(q.health().cause->kind == NodeCause::Kind::restarting);
		q.settle();
		CHECK(q.phase() == NodePhase::stopped);
		CHECK(q.node.intent() == Intent::none);
	}
	// A restart asked while the drive is in flight has begun its teardown (4.3's row 6), so a fault is dropped (I6), as
	// in fading: it never turns the restart into a failure (revision 2.3, found reviewing L-3a).
	SECTION("a fault during a restart's drive in flight is dropped, and the restart goes on") {
		Probe q = reach(Spot::startingDrive);
		q.change();
		CHECK(q.node.intent() == Intent::restart);
		q.take(q.node.fault(Fact::engine, "card gone"));
		CHECK(q.node.intent() == Intent::restart);
		CHECK(q.phase() == NodePhase::stopping);
		q.settle();
		CHECK(q.phase() == NodePhase::running);
	}
	SECTION("a fault during a stop's drive in flight is held, and fails the node once it rests") {
		Probe q = reach(Spot::startingDrive);
		q.take(q.node.stop());
		q.take(q.node.fault(Fact::engine, "card gone"));
		CHECK(q.node.heldFault() == Failure{FailureCode::engineRefused, "card gone"});
		q.settle();
		CHECK(q.phase() == NodePhase::failed);
	}
}

TEST_CASE("A stop after a completed restart is a plain stop", "[lifecycle][machines]") {
	Probe p = reach(Spot::running);
	p.take(p.node.restart());
	p.settle();
	CHECK(p.phase() == NodePhase::running);
	CHECK(p.node.intent() == Intent::none);
	p.clear();
	CHECK(p.take(p.node.stop()) == ok(LifecycleApplied::stop));
	CHECK(p.health().cause == NodeCause{NodeCause::Kind::stop, std::nullopt, {}});
	CHECK(p.node.configuration().state() == ConfigState::fading);
	p.settle();
	CHECK(p.phase() == NodePhase::stopped);
	CHECK_FALSE(p.did(K::teardown)); // a stop keeps the configuration
}

TEST_CASE("A change restarts a settled configuration and waits for an unsettled one", "[lifecycle][machines]") {
	SECTION("settled: running restarts at once with the changed keys") {
		Probe p = reach(Spot::running);
		p.change({"port", "rate"});
		CHECK(p.health().cause == NodeCause{NodeCause::Kind::restarting, NodeCause::Kind::settings, {"port", "rate"}});
		p.settle();
		CHECK(p.told.back().phase == NodePhase::running);
		CHECK(names(p.phases()) ==
		      std::vector<std::string_view>{"stopping", "configuring", "configured", "starting", "running"});
		CHECK(p.told[1].cause == NodeCause{NodeCause::Kind::settings, std::nullopt, {"port", "rate"}});
	}
	SECTION("unsettled: a restart's teardown takes the newest at its configure") {
		Probe p = reach(Spot::stoppingRestart);
		p.change({"a"});
		p.change({"b"});
		CHECK(p.node.intent() == Intent::restart);
		p.settle();
		CHECK(p.phase() == NodePhase::running);
		CHECK_FALSE(p.node.changePending());
	}
	SECTION("unsettled: a change during a stop's teardown, then a start, restarts first and drives the new settings "
	        "(D-L41)") {
		Probe p = reach(Spot::stoppingStop);
		p.change({"path"});
		p.take(p.node.start());
		p.step(); // faded
		p.step(); // silenced: rests, then the pending change restarts before any drive
		CHECK(p.node.intent() == Intent::restart);
		CHECK(p.node.configuration().state() == ConfigState::tearing);
		p.settle();
		CHECK(p.phase() == NodePhase::running);
		CHECK_FALSE(p.node.changePending());
		// One drive only: the new configuration's, after the stop's silence.
		std::size_t drives = 0;
		for (K k : p.asked) drives += k == K::drive;
		CHECK(drives == 1);
	}
	SECTION("a disabled node stores it") {
		Probe p = reach(Spot::disabled);
		p.change();
		CHECK(p.told.empty());
		CHECK(p.asked.empty());
	}
}

TEST_CASE("A failed node configures again only when a retry is asked", "[lifecycle][machines]") {
	Probe p = reach(Spot::failed);
	CHECK(p.node.configuration().state() == ConfigState::none);
	// Nothing but a retry configures it: not another block event, not a completion.
	p.take(p.node.blockReady());
	p.take(p.node.resourcesReady());
	p.take(p.node.closed());
	CHECK(p.phase() == NodePhase::failed);
	CHECK(p.asked.empty());

	SECTION("a retry during a failed node's teardown shows failed until its configure runs") {
		Probe q = reach(Spot::running);
		q.take(q.node.fault(Fact::nodeLost, "died"));
		CHECK(q.node.configuration().state() == ConfigState::fading);
		CHECK(q.take(q.node.start()) == ok(LifecycleApplied::retry));
		CHECK(q.node.retryAsked());
		CHECK(q.phase() == NodePhase::failed);
		q.step(); // faded
		q.step(); // silenced
		CHECK(q.phase() == NodePhase::failed);
		q.step(); // closed: the old configuration is gone, the retry configures (T15)
		CHECK(q.phase() == NodePhase::configuring);
		CHECK(q.health().cause == NodeCause{NodeCause::Kind::retry, std::nullopt, {}});
		CHECK_FALSE(q.node.failure().has_value());
		q.settle();
		CHECK(q.phase() == NodePhase::running);
	}
	SECTION("a stop on a failed node changes only wanted, and the retry then rests stopped") {
		Probe q = reach(Spot::failed);
		CHECK(q.take(q.node.stop()) == ok(LifecycleApplied::stop));
		REQUIRE(q.told.size() == 1);
		CHECK(q.told[0].phase == NodePhase::failed);
		CHECK(q.told[0].wanted == Wanted::stopped);
		CHECK(q.take(q.node.restart()) == ok(LifecycleApplied::retry));
		q.settle();
		CHECK(q.phase() == NodePhase::stopped);
	}
	SECTION("a start retries and clears the stop") {
		Probe q = reach(Spot::failed);
		q.take(q.node.stop());
		CHECK(q.take(q.node.start()) == ok(LifecycleApplied::retry));
		CHECK_FALSE(q.node.stopHeld());
		q.settle();
		CHECK(q.phase() == NodePhase::running);
	}
}

TEST_CASE("A fault during a stop's fade is held and fails the node once it rests, and is dropped during a teardown",
          "[lifecycle][machines]") {
	SECTION("held during a stop") {
		Probe p = reach(Spot::stoppingStop);
		p.take(p.node.fault(Fact::engine, "card unplugged"));
		CHECK(p.node.heldFault() == Failure{FailureCode::engineRefused, "card unplugged"});
		CHECK(p.phase() == NodePhase::stopping);
		p.step();                                       // faded
		p.take(p.node.fault(Fact::nodeLost, "second")); // the first held stays
		p.step();                                       // silenced: rests, then T14
		CHECK(p.phase() == NodePhase::failed);
		CHECK(p.health().reason == Failure{FailureCode::engineRefused, "card unplugged"});
		p.settle();
		CHECK(p.phase() == NodePhase::failed);
		CHECK_FALSE(p.told.empty());
		for (const Health& h : p.told) CHECK(h.phase != NodePhase::stopped); // never rests stopped with no card
	}
	SECTION("dropped during a teardown") {
		for (Spot spot : {Spot::stoppingRestart, Spot::stoppingDisable, Spot::stoppingRemove}) {
			INFO(spotName(spot));
			Probe p = reach(spot);
			p.take(p.node.fault(Fact::engine, "card unplugged"));
			CHECK_FALSE(p.node.heldFault().has_value());
			CHECK_FALSE(p.node.failure().has_value());
		}
	}
	SECTION("a restart over a held fault drops it at the configure") {
		Probe p = reach(Spot::stoppingStop);
		p.take(p.node.fault(Fact::engine, "card unplugged"));
		p.take(p.node.restart());
		p.settle();
		CHECK(p.phase() == NodePhase::stopped);
		CHECK_FALSE(p.node.heldFault().has_value());
	}
}

TEST_CASE("A block lost with no failure derives failed, and disabled with blockLost on a disabled node",
          "[lifecycle][machines]") {
	SECTION("not disabled") {
		Probe p = reach(Spot::running);
		p.take(p.node.blockLost("no driver"));
		p.settle();
		const Health h = p.health();
		CHECK(h.phase == NodePhase::failed);
		CHECK(h.reason == Failure{FailureCode::engineRefused, "no driver"});
		CHECK(h.blockLost);
		CHECK_FALSE(p.node.failure().has_value());
	}
	SECTION("disabled") {
		Probe p = reach(Spot::disabled);
		p.take(p.node.blockLost("no driver"));
		CHECK(p.health().phase == NodePhase::disabled);
		CHECK(p.health().blockLost);
		p.take(p.node.enable()); // back to failed, nothing configured
		CHECK(p.health().phase == NodePhase::failed);
		CHECK(p.health().reason == Failure{FailureCode::engineRefused, "no driver"});
		CHECK_FALSE(p.did(K::configure));
	}
	SECTION("the block's loss replaces a failure's reason, for good") {
		Probe p = reach(Spot::failed);
		p.take(p.node.blockLost("no driver"));
		CHECK(p.health().reason == Failure{FailureCode::engineRefused, "no driver"});
		p.take(p.node.blockReady());
		CHECK(p.health().blockLost);
	}
	SECTION("a block lost during a restart configures nothing again") {
		Probe p = reach(Spot::stoppingRestart);
		p.take(p.node.blockLost("no driver"));
		p.settle();
		CHECK(p.phase() == NodePhase::failed);
		std::size_t configures = 0;
		for (K k : p.asked) configures += k == K::configure;
		CHECK(configures == 0);
	}
}

TEST_CASE("A start() that threw is start-failed and pairs with no stop()", "[lifecycle][machines]") {
	Probe p;
	p.add();
	p.take(p.node.blockReady());
	p.take(p.node.resourcesReady());
	p.take(p.node.driven(p.node.configuration().barrier()));
	REQUIRE(p.phase() == NodePhase::running);
	p.clear();
	p.take(p.node.startThrew("start() threw: boom"));
	CHECK(p.health().reason == Failure{FailureCode::startFailed, "start() threw: boom"});
	CHECK_FALSE(p.did(K::stop));
	CHECK(p.did(K::fade));
}

TEST_CASE("The data loop wedged: a node added after it fails without a block, and a leaked stop fails the node",
          "[lifecycle][machines]") {
	SECTION("added after the wedge (D-L10)") {
		Probe p;
		p.add({.wedged = true});
		CHECK(p.blockAsked.empty());
		CHECK(p.asked.empty());
		CHECK(p.health().reason == Failure{FailureCode::engineRefused, std::string(wedgedDetail)});
	}
	SECTION("a silence overdue leaks the configuration and fails the node") {
		Probe p = reach(Spot::stoppingStop);
		p.step(); // faded
		p.take(p.node.barrierBound());
		CHECK(p.did(K::barrierOverdue));
		CHECK(p.did(K::markWedged));
		CHECK(p.did(K::leak));
		CHECK(p.phase() == NodePhase::failed);
		CHECK(p.health().reason == Failure{FailureCode::engineRefused, std::string(wedgedDetail)});
		CHECK(p.take(p.node.start()) == ok(LifecycleApplied::retry));
		CHECK(p.phase() == NodePhase::failed); // never configured again on a wedged loop
	}
	SECTION("a disable's silence overdue still ends disabled") {
		Probe p = reach(Spot::stoppingDisable);
		p.step();
		p.take(p.node.barrierBound());
		CHECK(p.phase() == NodePhase::disabled);
	}
	SECTION("another node's wedge stops a restart from configuring") {
		Probe p = reach(Spot::stoppingRestart);
		p.take(p.node.dataLoopWedged());
		p.settle();
		CHECK(p.phase() == NodePhase::failed);
		CHECK(p.health().reason == Failure{FailureCode::engineRefused, std::string(wedgedDetail)});
	}
}

TEST_CASE("A fade unacknowledged within its bound is silenced unfaded", "[lifecycle][machines]") {
	Probe p = reach(Spot::stoppingStop);
	p.take(p.node.fadeBound());
	CHECK(p.asked == std::vector<K>{K::fadeUnacknowledged, K::silence});
	p.settle();
	CHECK(p.phase() == NodePhase::stopped);
}

TEST_CASE("A close overdue is leaked, and a restart configures afresh after it", "[lifecycle][machines]") {
	Probe p = reach(Spot::stoppingRestart);
	p.step();
	p.step();
	REQUIRE(p.node.configuration().state() == ConfigState::tearing);
	p.take(p.node.closeBound());
	CHECK(p.did(K::closeOverdue));
	CHECK(p.did(K::leak));
	CHECK(p.phase() == NodePhase::configuring);
}

TEST_CASE("A port-caused replace starts in stopping (restarting) and tells no added", "[lifecycle][machines]") {
	Probe p;
	p.add({.stopHeld = true, .olderBlock = true, .predecessor = true, .keys = {"channels"}});
	REQUIRE(p.told.size() == 1);
	CHECK(p.told[0].phase == NodePhase::stopping);
	CHECK(p.told[0].cause == NodeCause{NodeCause::Kind::restarting, NodeCause::Kind::settings, {"channels"}});
	CHECK(p.told[0].wanted == Wanted::stopped); // the predecessor's stop carried (D-L36)
	CHECK(p.take(p.node.restart()) == ok(LifecycleApplied::unchanged));
	p.take(p.node.predecessorGone());
	p.take(p.node.olderDestroyed());
	CHECK(p.phase() == NodePhase::configuring);
	CHECK(p.health().cause == NodeCause{NodeCause::Kind::settings, std::nullopt, {"channels"}});
	p.settle();
	CHECK(p.phase() == NodePhase::stopped);
	for (const Health& h : p.told) CHECK(h.phase != NodePhase::added);

	SECTION("its block destroyed before its configuration closed") {
		Probe q;
		q.add({.olderBlock = true, .predecessor = true, .keys = {"channels"}});
		q.take(q.node.olderDestroyed());
		CHECK(q.didBlock(BlockAction::make));
		CHECK(q.phase() == NodePhase::stopping);
		CHECK_FALSE(q.did(K::configure));
		q.take(q.node.predecessorGone());
		CHECK(q.phase() == NodePhase::configuring);
		for (const Health& h : q.told) CHECK(h.phase != NodePhase::added);
	}
}

TEST_CASE("An enable during a disable's teardown configures afresh once the old configuration is gone",
          "[lifecycle][machines]") {
	Probe p = reach(Spot::stoppingDisable);
	p.take(p.node.enable());
	CHECK(p.node.intent() == Intent::restart);
	CHECK(p.health().cause == NodeCause{NodeCause::Kind::restarting, NodeCause::Kind::enabling, {}});
	p.settle();
	CHECK(names(p.phases()) ==
	      std::vector<std::string_view>{"stopping", "configuring", "configured", "starting", "running"});
	CHECK(p.told[1].cause == NodeCause{NodeCause::Kind::enabling, std::nullopt, {}});
}

TEST_CASE("Inputs after a node is removed change nothing", "[lifecycle][machines]") {
	Probe p = reach(Spot::disabled);
	p.take(p.node.remove());
	REQUIRE(p.gone);
	CHECK(p.node.enable().empty());
	CHECK(p.node.blockLost("x").empty());
	CHECK(p.node.change({9, true}, {}).empty());
	CHECK_FALSE(p.node.start().reply.has_value());
	CHECK(p.node.start().reply.error().code == LifecycleRefusal::Code::unknownNode);
}
