// What a node type's class is handed and answers (docs/writing-a-node-type.md#the-class, the type author's API), and
// the
// erased instance NodeHost drives (docs/writing-a-node-type.md#rules-the-framework-enforces).
//
// A type is one class, made a NodeType by makeNodeType<spec, Type>():
//
//   namespace ear::nodes::white {
//   inline constexpr auto noise = bank::output<"noise">;
//   inline constexpr TypeSpec spec{.settings = SettingsTable<>{"white"}, .banks = Banks<noise>{}};
//   struct White {
//   	static Members<spec> ports(const CheckedOf<spec>&) { return {noise(members("mono"))}; }
//   	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) { return cx.applied(WhiteCode{}); }
//   };
//   }
//
// The class's members, each found by name and held to exactly this at compile time (SpecInstance's checks):
//   static Members<spec> ports(const CheckedOf<spec>&);                                  required
//   static std::expected<void, std::string> check(const CheckedOf<spec>&);               optional, at parse
//   explicit Type(NodeOf<spec>&);  or  Type();                                           one of the two
//   Configured configure(CreateOf<spec>&, const CheckedOf<spec>&);                       required
//   Changed configure(ChangeOf<spec>&, const CheckedOf<spec>&, const CheckedOf<spec>&);  optional: a change in place
//   void start();  void stop();                                                          optional
//
// The change version is called for a change whose every key's row is marked inPlace, on a node that has rested
// (docs/architecture.md#changes-in-place); without one, every change of settings restarts the node.
//
// Every call here is on the main loop. CreateOf is valid only inside the configure it is handed to (a call after it
// returned is a std::logic_error naming the call, "<call> outside configure", thrown by NodeHost); NodeOf is valid for
// the instance's life. A rule the type breaks at a call or at configure's return fails the node start-failed with the
// rule's text, never escaping NodeHost. The bodies in this header are forwarding and compile-time checks only;
// NodeHost implements the erased contexts. SpecConfig, SpecNodeType and makeNodeType, at the end, make a type of the
// class (docs/writing-a-node-type.md#the-class).
#pragma once
#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "ear/core/companions.hpp"
#include "ear/core/loop.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/program.hpp"
#include "ear/core/recording.hpp"
#include "ear/core/sink.hpp"
#include "ear/core/source.hpp"
#include "ear/core/store.hpp"
#include "ear/core/type_spec.hpp"
#include "ear/core/report.hpp"
namespace ear {

// ---- Answers ----

class ErasedInstance;
template <const auto& Spec> class CreateOf;

/// One loop's state as an answer carries it: declared loop `loop` (its index in the spec's Loops) and its state, owned
/// from the answer on by the framework, which destroys it on the loop's thread at teardown step 4 (on the main loop if
/// the loop never started).
struct ErasedLoopState {
	std::size_t loop;
	std::unique_ptr<void, void (*)(void*)> state;
	template <class State> static ErasedLoopState of(std::size_t loop, std::unique_ptr<State> state) {
		return {loop, std::unique_ptr<void, void (*)(void*)>(state.release(),
		                                                     [](void* given) { delete static_cast<State*>(given); })};
	}
};

/// What creation's configure answers (docs/writing-a-node-type.md#the-class): a failure, or the code and the state of
/// every loop the spec declares with one. A type makes the applied answer only through its context
/// (`return cx.applied(code, states...)`), which holds it to its spec at compile time, so the code and each state are
/// given exactly once; an instance written by hand makes it through ErasedInstance::applied, and nothing else can. It
/// has no restart: that is the change version's answer.
class Configured {
public:
	/// A failure only the type can see: the node fails `start-failed: <detail>`, its configuration torn down.
	static Configured failed(std::string detail) {
		Configured answer;
		answer.failure_ = std::move(detail);
		return answer;
	}
	/// For the framework.
	bool isFailed() const noexcept { return failure_.has_value(); }
	/// For the framework; empty unless isFailed().
	const std::string& detail() const noexcept { return failure_ ? *failure_ : empty(); }
	/// For the framework, on an applied answer: its code and its states, each taken once.
	NodeCode& code() noexcept { return *code_; }
	std::vector<ErasedLoopState>& states() noexcept { return states_; }

private:
	friend class ErasedInstance;
	template <const auto&> friend class CreateOf;
	Configured() = default;
	/// The applied answer: the code, and one state for each loop the instance declares with one, by index.
	template <std::same_as<ErasedLoopState>... States> static Configured erased(NodeCode code, States... states) {
		Configured answer;
		answer.code_.emplace(std::move(code));
		answer.states_.reserve(sizeof...(States));
		(answer.states_.push_back(std::move(states)), ...);
		return answer;
	}
	static const std::string& empty() noexcept {
		static const std::string none;
		return none;
	}
	std::optional<std::string> failure_;
	std::optional<NodeCode> code_; // set unless failed
	std::vector<ErasedLoopState> states_;
};

template <const auto& Spec> class ChangeOf;

namespace detail {
/// Where a ConfigureDone answers: NodeHost's, posting the answer to the main loop through the change's configuration,
/// so an answer after the change ended (or the configuration did) lands nowhere. Any thread.
class ChangeAnswer {
public:
	virtual ~ChangeAnswer() = default;
	/// `failure` empty: done; else the change fails with it.
	virtual void answer(std::optional<std::string_view> failure) noexcept = 0;
};
} // namespace detail

/// A change in place's token (docs/writing-a-node-type.md#whenready-and-configuredone), handed to the continuation
/// whenReady posted: move-only; done() or fail(detail) once, from any thread. Destroyed without either (dropped, or
/// destroyed unrun with its work), it fails the change "the change was dropped". Once the change ended any other way,
/// both are ignored.
class ConfigureDone {
public:
	explicit ConfigureDone(std::unique_ptr<detail::ChangeAnswer> answer) noexcept : answer_(std::move(answer)) {}
	ConfigureDone(ConfigureDone&&) noexcept = default;
	ConfigureDone& operator=(ConfigureDone&& other) noexcept {
		if (this != &other) {
			drop();
			answer_ = std::move(other.answer_);
		}
		return *this;
	}
	ConfigureDone(const ConfigureDone&) = delete;
	ConfigureDone& operator=(const ConfigureDone&) = delete;
	~ConfigureDone() { drop(); }
	/// The continuation's work is done: the change goes on to its commit point.
	void done() && {
		if (const auto answer = std::exchange(answer_, nullptr)) answer->answer(std::nullopt);
	}
	/// The change cannot be made: it falls back to a restart with the new settings (reason failed, `detail` its words).
	void fail(std::string detail) && {
		if (const auto answer = std::exchange(answer_, nullptr)) answer->answer(std::string_view(detail));
	}

private:
	void drop() noexcept {
		if (const auto answer = std::exchange(answer_, nullptr))
			answer->answer(std::string_view("the change was dropped"));
	}
	std::unique_ptr<detail::ChangeAnswer> answer_;
};

/// What the change version of configure answers (docs/writing-a-node-type.md#the-change-version): applied, with or
/// without a new code, made only by its context (`cx.applied(...)`), or a restart the type asks for when it cannot take
/// this change in place after all, which falls the change back to a restart with the new settings.
class Changed {
public:
	/// The type cannot take this change in place: the node restarts with the new settings (reason restart-asked,
	/// `reason` its words).
	static Changed restart(std::string reason) {
		Changed answer;
		answer.restart_ = std::move(reason);
		return answer;
	}
	/// For the framework: whether the answer is a restart, and its reason.
	bool restarts() const noexcept { return restart_.has_value(); }
	const std::string& reason() const noexcept { return restart_ ? *restart_ : empty(); }
	/// For the framework, on an applied answer: the new code, if one was given (taken once).
	std::optional<NodeCode>& code() noexcept { return code_; }
	/// For the framework: the answer whenReady gave, which goes on in its continuation.
	bool continues() const noexcept { return continues_; }

private:
	friend class ErasedInstance;
	template <const auto&> friend class ChangeOf;
	Changed() = default;
	static Changed continued() {
		Changed answer;
		answer.continues_ = true;
		return answer;
	}
	static Changed erased(std::optional<NodeCode> code) {
		Changed answer;
		answer.code_ = std::move(code);
		return answer;
	}
	static const std::string& empty() noexcept {
		static const std::string none;
		return none;
	}
	std::optional<std::string> restart_;
	std::optional<NodeCode> code_;
	bool continues_ = false;
};

// ---- The erased contexts NodeHost implements ----

/// What NodeOf<Spec> wraps: the node's own, for one configuration's instance. NodeHost implements it on the
/// configuration, declared before the instance, so it outlives the instance. Main thread, but handle(), whose result
/// any of the node's threads may keep and call until the configuration's teardown ended (node_handle.hpp).
class NodeContext {
public:
	virtual AudioFormat format() const noexcept = 0;
	virtual const NodeName& node() const noexcept = 0;
	/// The configuration's handle: its Channel's public face.
	virtual NodeHandle& handle() noexcept = 0;
	/// Work for declared loop `loop` (its index in the spec's Loops), called with its state (null for a loop with
	/// none) on the loop's thread; staged until the node is configured (docs/writing-a-node-type.md#loops). Main
	/// thread.
	virtual void postToLoop(std::size_t loop, std::move_only_function<void(void*)> work) = 0;

protected:
	~NodeContext() = default;
};

/// A sink's consumer that is part of a loop's state (CreateOf::attach's loop forms for a sink): declared loop `loop` (its index in
/// the spec's Loops), `member` finding the consumer in the state when the consumer begins. The state owns it: the sink
/// closes (teardown step 3) before the state is destroyed (step 4), and a sink whose loop never ran never begins.
struct StateSinkConsumer {
	std::size_t loop;
	std::move_only_function<SinkConsumer&(void*)> member;
};
/// A sink's one consumer, as CreateContext::sink takes it: the type's own, given whole and owned by the sink (file's
/// WavWriter), or a part of a loop's state.
using SinkConsumerFrom = std::variant<std::unique_ptr<SinkConsumer>, StateSinkConsumer>;

/// Where a program's report feed goes (CreateOf::program's feed form): declared loop `loop`'s state, `deliver` called on
/// the loop's thread with the state and each chunk read, in order.
struct ProgramFeedTo {
	std::size_t loop;
	std::move_only_function<void(void*, std::string_view)> deliver;
};

/// What creation's configure acquires through: the erased side of CreateOf<Spec>. NodeHost implements it on the node,
/// valid only inside the configure it is handed to; any call outside it throws std::logic_error. Banks arrive by name,
/// already checked against the spec at compile time; NodeHost re-checks each bank's reach and direction (a
/// std::logic_error naming the bank otherwise).
class CreateContext {
public:
	virtual AudioFormat format() const = 0;
	virtual const NodeName& node() const = 0;
	/// The node's ports (its config's), to bind a code written against the spec.
	virtual const Ports& ports() const = 0;
	/// The configuration's realtime channels (docs/architecture.md#the-realtime-channels), to bind the code to; null
	/// for a type whose code writes nothing on the realtime thread.
	virtual RealtimeChannels* realtime() const = 0;
	/// An engine companion on an inside bank (docs/writing-a-node-type.md#engine-companions), its kind its shape's: a
	/// sound card (playback on an inside output bank, `<node>.playback`, capture on an inside input bank,
	/// `<node>.capture`: the bank's direction is the card's, and a playback card's bank is faded with the public
	/// outputs), an RTP receiver on an inside input bank (`<node>.rtp`, never faded, its `latencyMs` latency beyond the
	/// bank: LatencyPlan::beyond) or a pipe receiver on an inside input bank (`<node>.pipe`, never faded, reading the
	/// FIFO runtimePath(<runtime folder>, node, <configuration>, "<bank>.fifo"), its module's fixed buffer latency
	/// beyond the bank: moduleRunDelay). Made once the block is ready, in the turn's flush. A bank of the wrong reach
	/// or direction, or a second companion on the bank, is a std::logic_error naming it; a host with no adapter of the
	/// kind (or, for a pipe receiver, no runtime folder) refuses it. Returns a pipe receiver's FIFO, as a part of a
	/// program's argument or file; nothing for the other kinds.
	virtual std::optional<part::Fifo> attachEngine(std::string_view bank, EngineCompanionShape shape) = 0;
	/// A recording file beneath the recordings folder (docs/writing-a-node-type.md#recordings): claimed
	/// ("recording:<path>") at the first readiness step, created or truncated on the I/O thread at step `opens`, its
	/// descriptor valid from the sink consumer's begin() until the configuration is freed (teardown step 5), when it is
	/// closed on the I/O thread. A refusal fails the node start-failed "cannot open <path>: <why>" at its step; one path
	/// asked for twice in one configuration is a std::logic_error.
	virtual Recording& recording(const RecordingRequest& request) = 0;
	/// A sink on an inside output bank (docs/engine.md#sinks), with its one consumer: made at once (its thread starts);
	/// its consumer begins once configure answered applied (readiness 4'). A null consumer, or one in the state of a
	/// loop declared without a state, is a std::logic_error naming the sink's bank, and no sink is made.
	virtual SinkControl& sink(std::string_view bank, SinkShape shape, SinkConsumerFrom consumer) = 0;
	/// A source on an inside input bank (docs/writing-a-node-type.md#sources): made at once and ready at once; the block
	/// plays it into the bank before the code every cycle the code runs; its ring's capacity is declared beyond the bank
	/// (LatencyPlan::beyond, Latency{0, capacity}). A second companion on the bank, `shape.frames == 0` or a call outside
	/// configure is a std::logic_error; an adapter that cannot make it fails the node start-failed. The writer is valid
	/// until the configuration is freed (teardown step 5), closed at teardown step 1. `clock` (null for a source with no
	/// clock) is the source's own, owned by it and freed with it at teardown step 5.
	virtual SourceWriter& source(std::string_view bank, SourceShape shape, std::unique_ptr<SourceClock> clock) = 0;
	/// The type's own code's latency (docs/engine.md#latency-per-bank-pair): `latency` on the path from `from` to `to`.
	/// The plan carries that path (LatencyPlan::pairs, added for a code of the type's own) with the delay on it
	/// (LatencyPlan::onPair), so it counts both ways: in what `to` tells downstream and in what `from` tells upstream.
	/// Keyed by the path, so a code with several inputs into one output (a mixer) declares each path: a second for the
	/// same path is a std::logic_error. With a pass-through as the code, the path must be one of its pairs, or the
	/// node fails at configure's return.
	virtual void declareLatency(std::string_view from, std::string_view to, Latency latency) = 0;
	/// Declared loop `loop` as an executor, for a state to post to and arm on; valid until the configuration is freed
	/// (teardown step 5). What is sent to it before the node is configured is staged.
	virtual Loop& loopExecutor(std::size_t loop) = 0;
	/// As NodeContext::postToLoop, staged.
	virtual void postToLoop(std::size_t loop, std::move_only_function<void(void*)> work) = 0;
	/// Declared store `store` (its index in the spec's Stores; docs/writing-a-node-type.md#stores): opened at readiness
	/// step `opens` whether or not this is called, valid until the configuration is freed (teardown step 5).
	virtual Store& store(std::size_t store) = 0;
	/// A port the type's own thread will bind (docs/writing-a-node-type.md#claims): judged with the configuration's other
	/// claims at the first readiness step, before anything is opened, held until teardown step 5. A claim of another
	/// kind (a store's or a recording's, which the framework claims itself) is a std::logic_error.
	virtual void claim(Claim claim) = 0;
	/// A listening TCP port the framework picks at the first readiness step (docs/writing-a-node-type.md#claims), held as
	/// a TcpListen of it once granted: its handle, the claim's index among the configuration's claims, in asking order.
	virtual GrantedPort claimAnyTcp() = 0;
	/// The configuration's one program (docs/writing-a-node-type.md#programs), started at the last readiness step;
	/// `feed`, when given, where its report feed goes. A rule the shape breaks is a std::logic_error at the call.
	virtual void program(ProgramShape shape, std::optional<ProgramFeedTo> feed) = 0;

protected:
	~CreateContext() = default;
	/// The FIFO of the pipe receiver on `bank`: what attachEngine returns for one, and nothing else makes.
	static part::Fifo fifoOf(std::string bank) { return part::Fifo(std::move(bank)); }
};

/// Which goes first when a change replaces an engine companion (docs/writing-a-node-type.md#make-before-break): the new
/// one made beside the old one (makeFirst), or the old one destroyed and the new one made in one flush (breakFirst:
/// one ALSA device opened twice, say).
enum class Order { makeFirst, breakFirst };

/// What the change version of configure works through: the erased side of ChangeOf<Spec>
/// (docs/writing-a-node-type.md#the-change-version). NodeHost implements it for one change, valid only inside the
/// configure it is handed to; a call outside it throws std::logic_error. Banks arrive by name, already checked against
/// the spec at compile time.
class ChangeContext {
public:
	virtual AudioFormat format() const = 0;
	virtual const NodeName& node() const = 0;
	/// The node's ports, to bind a new code written against the spec.
	virtual const Ports& ports() const = 0;
	/// The configuration's realtime channels, to bind a new code to the same ones its code writes.
	virtual RealtimeChannels* realtime() const = 0;
	/// Work for declared loop `loop`, staged until the commit point and destroyed unrun if the change ends any other way.
	virtual void postToLoop(std::size_t loop, std::move_only_function<void(void*)> work) = 0;
	/// A replacement for the engine companion on `bank` (an RTP receiver or a sound card), behind a hold the framework
	/// takes, made before break unless `order` says break-first (an RTP receiver on the same port is break-first
	/// whatever it says): docs/writing-a-node-type.md#make-before-break.
	virtual void attachEngine(std::string_view bank, EngineCompanionShape shape, Order order) = 0;
	/// A replacement for the sink on `bank`, made at once beside it, its consumer begun before the commit point; the
	/// block's tap moves to it in the commit's invoke, and the old one drains to its own consumer and finishes after it
	/// (docs/writing-a-node-type.md#sinks-and-sources-in-a-change). Its rules are CreateContext::sink's.
	virtual SinkControl& sink(std::string_view bank, SinkShape shape, SinkConsumerFrom consumer) = 0;
	/// A replacement for the source on `bank`, made at once (its writer valid at once) and played from the commit's
	/// invoke; the old one's writer is closed at the commit point. Its rules are CreateContext::source's.
	virtual SourceWriter& source(std::string_view bank, SourceShape shape, std::unique_ptr<SourceClock> clock) = 0;
	/// A replacement for `replaced`, a recording the configuration holds: claimed and opened beside it, before the
	/// change's companions; the old one closed once the commit point completed and every sink the change replaced
	/// finished (docs/writing-a-node-type.md#recordings-in-a-change).
	virtual Recording& recording(const Recording& replaced, const RecordingRequest& request) = 0;
	/// Routable output banks to hold silent from the next cycle, after a fade, until the commit point (or release).
	virtual void hold(std::span<const std::string_view> banks) = 0;
	virtual void holdAll() = 0;
	virtual void release(std::span<const std::string_view> banks) = 0;
	/// The new configuration's latency on the path from `from` to `to`, carried from the commit point.
	virtual void declareLatency(std::string_view from, std::string_view to, Latency latency) = 0;
	/// A port the type's own thread will bind once the change is made (docs/writing-a-node-type.md#claims-in-a-change):
	/// judged when the change version returns, before any other acquisition; a key the configuration holds is its own.
	virtual void claim(Claim claim) = 0;
	/// A port the configuration holds that it no longer needs: released at the commit point, last; a key the change
	/// also claims stays held. A key it does not hold is a std::logic_error.
	virtual void unclaim(Claim claim) = 0;
	/// The change's one later answer (docs/writing-a-node-type.md#whenready-and-configuredone): `work`, called with
	/// declared loop `loop`'s state (null for a loop with none) and the change's token, is posted to that loop once
	/// every acquisition of the change is ready and every hold acknowledged; destroyed unrun if the change ends before.
	/// A second call is a std::logic_error.
	virtual void whenReady(std::size_t loop, std::move_only_function<void(void*, ConfigureDone)> work) = 0;

protected:
	~ChangeContext() = default;
};

// ---- A companion's bank ----

/// The bank a companion of each shape joins (docs/writing-a-node-type.md#engine-companions, #sinks, #sources): the
/// shape's one rule, checked at compile time by the context's attach. A shape with no rule is no companion's.
template <class Shape> struct CompanionBank;
template <> struct CompanionBank<SoundCardShape> {
	template <class Bank> static constexpr void check() noexcept {
		static_assert(Bank::reach == PortBank::Reach::inside, "a sound card joins an inside bank of the spec");
	}
};
template <> struct CompanionBank<RtpReceiverShape> {
	template <class Bank> static constexpr void check() noexcept {
		static_assert(Bank::reach == PortBank::Reach::inside && Bank::direction == Direction::input,
		              "an RTP receiver joins an inside input bank of the spec");
	}
};
template <> struct CompanionBank<PipeReceiverShape> {
	template <class Bank> static constexpr void check() noexcept {
		static_assert(Bank::reach == PortBank::Reach::inside && Bank::direction == Direction::input,
		              "a pipe receiver joins an inside input bank of the spec");
	}
};
template <> struct CompanionBank<SourceShape> {
	template <class Bank> static constexpr void check() noexcept {
		static_assert(Bank::reach == PortBank::Reach::inside && Bank::direction == Direction::input,
		              "a source plays into an inside input bank of the spec");
	}
};
template <> struct CompanionBank<SinkShape> {
	template <class Bank> static constexpr void check() noexcept {
		static_assert(Bank::reach == PortBank::Reach::inside && Bank::direction == Direction::output,
		              "a sink reads an inside output bank of the spec");
	}
};
/// Whether `Shape` is a companion's: it states its bank.
template <class Shape>
concept CompanionShape = requires { sizeof(CompanionBank<Shape>); };

// ---- The type's views of them ----

namespace spec_detail {
template <const auto& Spec, class Type> class SpecInstance;
/// A post to a loop as the erased contexts take it: `work` called with the loop's state, or with nothing for a loop with
/// none. A function of another loop's state does not compile.
template <class Handle, class Work> std::move_only_function<void(void*)> loopWork(Work&& work) {
	using State = typename Handle::State;
	using Stored = std::decay_t<Work>;
	if constexpr (Handle::stateful) {
		static_assert(std::is_invocable_v<Stored&, State&>, "a post to a loop takes a function of that loop's state");
		return [work = Stored(std::forward<Work>(work))](void* state) mutable { work(*static_cast<State*>(state)); };
	} else {
		static_assert(std::is_invocable_v<Stored&>, "a post to a loop without a state takes a function of nothing");
		return [work = Stored(std::forward<Work>(work))](void*) mutable { work(); };
	}
}
} // namespace spec_detail

/// What the instance keeps: the node's own, for the instance's life, main loop only (handle() excepted, as
/// NodeContext's): docs/writing-a-node-type.md#the-class.
template <const auto& Spec> class NodeOf {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");

public:
	NodeOf(const NodeOf&) = delete;
	NodeOf& operator=(const NodeOf&) = delete;
	AudioFormat format() const noexcept { return context_->format(); }
	const NodeName& node() const noexcept { return context_->node(); }
	/// The configuration's handle (node_handle.hpp: say, lost, log); keep it in a sink's consumer or a thread the
	/// instance owns.
	NodeHandle& handle() noexcept { return context_->handle(); }
	/// The configuration's Report (docs/writing-a-node-type.md#report): its status writes and events, by the spec's
	/// handles. Copy it into the loop states that write them.
	Report<Spec> report() noexcept { return Report<Spec>(context_->handle()); }
	/// `work(state)` on loop `Handle`'s thread (`work()` for a loop with no state), in post order; staged until the
	/// node is configured (docs/writing-a-node-type.md#loops). The instance's only way to its loops' states: start()
	/// and stop() post. A loop the spec lacks, or a function of another loop's state, does not compile.
	template <class Handle, class Work> void post(Handle, Work&& work) {
		static_assert(isLoopHandle<Handle> && LoopsOf<Spec>::template has<Handle>,
		              "a post names one of its spec's loops");
		context_->postToLoop(LoopsOf<Spec>::template indexOf<Handle>,
		                     spec_detail::loopWork<Handle>(std::forward<Work>(work)));
	}

private:
	template <const auto&, class> friend class spec_detail::SpecInstance;
	explicit NodeOf(NodeContext& context) noexcept : context_(&context) {}
	NodeContext* context_;
};

/// A loop's state on its way into configure's answer (docs/writing-a-node-type.md#loops): made only by
/// `cx.state(loop, args...)`, which constructs it, and given to `cx.applied`. Move-only; tagged with its loop, so the
/// answer's check is on types alone.
template <class Handle> class LoopState {
public:
	using Loop = Handle;
	LoopState(LoopState&&) noexcept = default;
	LoopState& operator=(LoopState&&) = delete;

private:
	template <const auto&> friend class CreateOf;
	explicit LoopState(std::unique_ptr<typename Handle::State> state) noexcept : state_(std::move(state)) {}
	std::unique_ptr<typename Handle::State> state_;
};
template <class T> inline constexpr bool isLoopState = false;
template <class Handle> inline constexpr bool isLoopState<LoopState<Handle>> = true;

namespace spec_detail {
/// How many of the loops `Given` is `Handle`.
template <class Handle, class... Given> inline constexpr std::size_t statesFor =
	(std::size_t{0} + ... + std::size_t{std::same_as<Handle, Given>});
/// One check for each loop of a spec, so the compiler's error names the loop (its instantiation's `Handle`).
template <class Handle, class... Given> constexpr void stateGivenOnce() noexcept {
	static_assert(!Handle::stateful || statesFor<Handle, Given...> >= 1,
	              "configure's answer gives the state of every loop its spec declares with one: "
	              "cx.applied(code, cx.state(loop, ...))");
	static_assert(statesFor<Handle, Given...> <= 1, "configure's answer gives a loop's state once");
}
template <class List, class... Given> struct StatesGiven;
template <auto... Handles, class... Given> struct StatesGiven<Loops<Handles...>, Given...> {
	static constexpr void check() noexcept {
		(stateGivenOnce<std::remove_cvref_t<decltype(Handles)>, Given...>(), ...);
	}
};
} // namespace spec_detail

/// configure on creation (T2): acquire, construct and answer (docs/writing-a-node-type.md#the-class). Only the spec's
/// banks, of the right direction and reach, compile. It has no holds, whenReady or unclaim (the change version's), so a
/// call the mode does not offer does not compile.
template <const auto& Spec> class CreateOf {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
	using List = BanksOf<Spec>;
	template <class Bank> static constexpr bool ownBank = isBankHandle<Bank> && List::template has<Bank>;
	template <class Handle> static constexpr bool ownLoop = isLoopHandle<Handle> && LoopsOf<Spec>::template has<Handle>;
	template <class Handle> static constexpr bool ownStore =
		isStoreHandle<Handle> && StoresOf<Spec>::template has<Handle>;

public:
	CreateOf(const CreateOf&) = delete;
	CreateOf& operator=(const CreateOf&) = delete;
	AudioFormat format() const { return context_->format(); }
	const NodeName& node() const { return context_->node(); }

	// ---- The answer ----

	/// The applied answer (docs/writing-a-node-type.md#configure-and-createof): the code, by value (a struct with
	/// process, or passThrough(...)), and the state of every loop the spec declares with one, each made by state() and
	/// given once, in any order. A state missing, a second for one loop, a code given as a pointer or one written
	/// against another type's spec (it would read this node's ports by that spec's banks) does not compile. A code
	/// written against the spec is bound to the node's ports here.
	template <class Code, class... States> Configured applied(Code code, States... states) {
		static_assert(SpecCode<Code> || std::same_as<Code, PassThroughPairs>,
		              "configure answers with its code by value: a struct with process, or passThrough(...)");
		static_assert((isLoopState<States> && ...),
		              "configure's answer is its code, then its loops' states, each made by cx.state(loop, ...)");
		if constexpr ((isLoopState<States> && ...)) {
			spec_detail::StatesGiven<LoopsOf<Spec>, typename States::Loop...>::check();
			// What only a moved-from token can hold, and so only at run time: the type's bug, thrown inside its
			// configure so the node fails with it (docs/writing-a-node-type.md#rules-the-framework-enforces).
			if constexpr (std::same_as<Code, PassThroughPairs>) {
				if (code.pairs().empty()) throw std::logic_error("applied given a pass-through that was moved from");
			}
			const auto given = [](std::string_view loop, bool held) {
				if (!held)
					throw std::logic_error("applied given a state of loop " + std::string(loop) +
					                       " that was moved from");
			};
			(given(States::Loop::name, states.state_ != nullptr), ...);
			NodeCode erased(std::move(code));
			if constexpr (SpecCode<Code>) {
				static_assert(static_cast<const void*>(&spec_detail::ProcessOf<decltype(&Code::process)>::spec) ==
				                  static_cast<const void*>(&Spec),
				              "configure answers with a code written against its own type's spec");
				static_cast<spec_detail::BankBoundCode&>(*erased.own()).bind(context_->ports(), context_->realtime());
			} else {
				static_assert(!spec_detail::needsCode<Spec>,
				              "a spec with writer::rt fields, log codes or triggers answers with "
				              "a code of its own, never a pass-through");
			}
			return Configured::erased(std::move(erased),
			                          ErasedLoopState::of(LoopsOf<Spec>::template indexOf<typename States::Loop>,
			                                              std::move(states.state_))...);
		} else {
			return Configured::failed({});
		}
	}
	/// Loop `Handle`'s state (docs/writing-a-node-type.md#loops), constructed here from `args` on the main loop with
	/// no I/O and no threads, for the answer: once the answer is given the framework owns it, and the instance reaches
	/// it only by a post. A loop the spec lacks, or one declared without a state, does not compile.
	template <class Handle, class... Args> LoopState<Handle> state(Handle, Args&&... args) {
		static_assert(ownLoop<Handle>, "a context takes only its spec's loops");
		static_assert(Handle::stateful, "a loop declared without a state takes none");
		return LoopState<Handle>(std::make_unique<typename Handle::State>(std::forward<Args>(args)...));
	}

	// ---- Companions: one per inside bank, attached by its shape (CompanionBank states each shape's bank) ----

	/// A sound card on an inside bank (docs/writing-a-node-type.md#engine-companions): playback on an output bank,
	/// capture on an input bank (the bank's direction is the card's, so the shape names none).
	template <class Bank> void attach(Bank, SoundCardShape shape) {
		checkCompanion<Bank, SoundCardShape>();
		context_->attachEngine(Bank::name, std::move(shape));
	}
	/// An RTP receiver on an inside input bank.
	template <class Bank> void attach(Bank, RtpReceiverShape shape) {
		checkCompanion<Bank, RtpReceiverShape>();
		context_->attachEngine(Bank::name, std::move(shape));
	}
	/// A pipe receiver on an inside input bank: an engine companion, `<node>.pipe`, reading a FIFO the framework
	/// names, which a program of the configuration writes. Its module's fixed buffer is latency beyond the bank
	/// (moduleRunDelay); never faded. Returns that FIFO, as a part of a program's argument or file
	/// (docs/writing-a-node-type.md#programs): attaching the receiver is the only way to one.
	template <class Bank> part::Fifo attach(Bank, PipeReceiverShape shape) {
		checkCompanion<Bank, PipeReceiverShape>();
		return context_->attachEngine(Bank::name, std::move(shape)).value();
	}
	/// A source on an inside input bank (docs/writing-a-node-type.md#sources): what the type's ordinary thread writes,
	/// played into the bank by the block before the code; `clock` its own clock, null for a source with no clock
	/// (docs/writing-a-node-type.md#a-sources-clock).
	template <class Bank> SourceWriter& attach(Bank, SourceShape shape, std::unique_ptr<SourceClock> clock = nullptr) {
		checkCompanion<Bank, SourceShape>();
		return context_->source(Bank::name, shape, std::move(clock));
	}
	/// A sink on an inside output bank with its one consumer (docs/writing-a-node-type.md#sinks): here a consumer of
	/// the type's own, given whole and owned by the sink (file's WavWriter). Its shape is named (deduced), so a braced
	/// shape with a null third argument is a source with no clock, never a sink with no consumer.
	template <class Bank, std::same_as<SinkShape> Shape>
	SinkControl& attach(Bank, Shape shape, std::unique_ptr<SinkConsumer> consumer) {
		checkCompanion<Bank, SinkShape>();
		return context_->sink(Bank::name, shape, std::move(consumer));
	}
	/// A sink with no consumer does not compile: nothing would ever read it. Its shape is deduced, so a braced shape
	/// (`cx.attach(bank, {.frames = n})`, a source's) never selects this refusal.
	template <class Bank, std::same_as<SinkShape> Shape> void attach(Bank, Shape) {
		static_assert(sizeof(Bank) == 0, "a sink is given its one consumer: cx.attach(bank, shape, consumer)");
	}
	/// The sink's consumer is `member` of loop `Handle`'s state (`cx.attach(bank, shape, worker, &Worker::toServer)`):
	/// it lives as long as the state, which outlives the sink's last call. Its calls still run on the sink's own
	/// thread, never the loop's. A loop the spec lacks, a loop without a state, or a member that is no SinkConsumer
	/// does not compile.
	template <class Bank, class Handle, class State, class Member>
		requires isLoopHandle<Handle>
	SinkControl& attach(Bank, SinkShape shape, Handle, Member State::* member) {
		checkCompanion<Bank, SinkShape>();
		static_assert(ownLoop<Handle>, "a sink is consumed by one of its spec's loops");
		// The member may be one the state inherits (`&Worker::toServer` then names its base), so the state is cast to
		// its own type first.
		static_assert(Handle::stateful && std::derived_from<typename Handle::State, State>,
		              "a sink's consumer is a member of that loop's state");
		static_assert(std::derived_from<Member, SinkConsumer>, "a sink's consumer is a SinkConsumer");
		if constexpr (Handle::stateful && std::derived_from<typename Handle::State, State> &&
		              std::derived_from<Member, SinkConsumer>) {
			using Own = typename Handle::State;
			return context_->sink(
				Bank::name, shape,
				StateSinkConsumer{LoopsOf<Spec>::template indexOf<Handle>, [member](void* state) -> SinkConsumer& {
									  return static_cast<Own*>(state)->*member;
								  }});
		}
	}
	/// The sink's consumer is loop `Handle`'s state itself.
	template <class Bank, class Handle>
		requires isLoopHandle<Handle>
	SinkControl& attach(Bank, SinkShape shape, Handle) {
		checkCompanion<Bank, SinkShape>();
		static_assert(ownLoop<Handle>, "a sink is consumed by one of its spec's loops");
		using State = typename Handle::State;
		static_assert(Handle::stateful && std::derived_from<State, SinkConsumer>,
		              "a loop's state that consumes a sink is a SinkConsumer");
		return context_->sink(
			Bank::name, shape,
			StateSinkConsumer{LoopsOf<Spec>::template indexOf<Handle>,
		                      [](void* state) -> SinkConsumer& { return *static_cast<State*>(state); }});
	}
	/// What is no companion's shape does not compile.
	template <class Bank, class Shape, class... Rest>
		requires(!CompanionShape<std::remove_cvref_t<Shape>>)
	void attach(Bank, Shape&&, Rest&&...) {
		static_assert(CompanionShape<std::remove_cvref_t<Shape>>,
		              "attach takes a companion's shape: a sound card's, an RTP receiver's, a pipe receiver's, a "
		              "source's or a sink's");
	}

	/// A recording file beneath the recordings folder (docs/writing-a-node-type.md#recordings): give it to the sink's
	/// consumer, which reads its descriptor from begin() on. Its refusal is the framework's: the node fails, and
	/// configure answers nothing for it.
	Recording& recording(RecordingRequest request) { return context_->recording(request); }
	/// The type's own code's latency, from an input bank to an output bank (it lands on `To`).
	template <class From, class To> void declareLatency(From, To, Latency latency) {
		static_assert(ownBank<From> && ownBank<To>, "declareLatency takes its spec's banks");
		static_assert(From::direction == Direction::input && To::direction == Direction::output,
		              "declareLatency names a pair: an input bank, then an output bank");
		context_->declareLatency(From::name, To::name, latency);
	}

	/// Loop `Handle` as an executor (post, timers, watches) for a state to keep: valid until the configuration is freed;
	/// what is sent to it before the node is configured is staged.
	template <class Handle> Loop& executor(Handle) {
		static_assert(ownLoop<Handle>, "a context takes only its spec's loops");
		return context_->loopExecutor(LoopsOf<Spec>::template indexOf<Handle>);
	}
	/// As NodeOf::post, staged until the node is configured.
	template <class Handle, class Work> void post(Handle, Work&& work) {
		static_assert(ownLoop<Handle>, "a context takes only its spec's loops");
		context_->postToLoop(LoopsOf<Spec>::template indexOf<Handle>,
		                     spec_detail::loopWork<Handle>(std::forward<Work>(work)));
	}

	/// Store `Handle` (docs/writing-a-node-type.md#stores), for a loop's state or a consumer to keep: valid until the
	/// configuration is freed, open and trusted once the node is configured. Every declared store is opened, kept or
	/// not. A store the spec lacks does not compile.
	template <class Handle> Store& store(Handle) {
		static_assert(ownStore<Handle>, "a context takes only its spec's stores");
		return context_->store(StoresOf<Spec>::template indexOf<Handle>);
	}

	/// A listening TCP port or a UDP port the type's own thread binds (docs/writing-a-node-type.md#claims): one holder
	/// ear-wide, judged at the first readiness step and probed against the kernel, so a port another node or program
	/// holds fails the node start-failed before anything of it is opened or started.
	void claim(claim::TcpListen port) { context_->claim(claim::of(port)); }
	void claim(claim::Udp port) { context_->claim(claim::of(port)); }
	/// A listening TCP port nobody holds, picked by the framework at the first readiness step (claim::AnyTcp): its
	/// handle, which names the port in a program's arguments or files (part::Port) before it is picked.
	GrantedPort claim(claim::AnyTcp) { return context_->claimAnyTcp(); }

	/// A program (docs/writing-a-node-type.md#programs), started once every companion is ready, with no report feed.
	void program(ProgramShape shape) { context_->program(std::move(shape), std::nullopt); }
	/// A program whose report feed (part::Feed in its arguments or files) reaches loop `Handle`'s state: `onFeed`
	/// called on the loop's thread with each chunk read, in order, from the node's configuring on (staged until the
	/// node is configured, as every post is). A loop the spec lacks, or one declared without a state, does not compile.
	template <class Handle>
	void program(ProgramShape shape, Handle, void (Handle::State::*onFeed)(std::string_view bytes)) {
		static_assert(ownLoop<Handle>, "a context takes only its spec's loops");
		static_assert(Handle::stateful, "a program's feed reaches a loop declared with a state");
		using State = typename Handle::State;
		context_->program(std::move(shape), ProgramFeedTo{LoopsOf<Spec>::template indexOf<Handle>,
		                                                  [onFeed](void* state, std::string_view bytes) {
															  (static_cast<State*>(state)->*onFeed)(bytes);
														  }});
	}

private:
	template <const auto&, class> friend class spec_detail::SpecInstance;
	explicit CreateOf(CreateContext& context) noexcept : context_(&context) {}
	template <class Bank, class Shape> static constexpr void checkCompanion() noexcept {
		static_assert(ownBank<Bank>, "a context takes only its spec's banks");
		CompanionBank<Shape>::template check<Bank>();
	}
	CreateContext* context_;
};

/// configure for a change in place (docs/writing-a-node-type.md#the-change-version): holds, replacement companions and
/// recordings, claims and unclaims, staged posts, the latency and the answer. It has no state, store, executor or program (a loop's
/// state lives across a change and already holds what it needs), so a call the mode does not offer does not compile.
template <const auto& Spec> class ChangeOf {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
	using List = BanksOf<Spec>;
	template <class Bank> static constexpr bool ownBank = isBankHandle<Bank> && List::template has<Bank>;
	template <class Handle> static constexpr bool ownLoop = isLoopHandle<Handle> && LoopsOf<Spec>::template has<Handle>;
	template <class Bank> static constexpr bool routableOutput =
		ownBank<Bank> && Bank::direction == Direction::output && Bank::reach == PortBank::Reach::routable;

public:
	ChangeOf(const ChangeOf&) = delete;
	ChangeOf& operator=(const ChangeOf&) = delete;
	AudioFormat format() const { return context_->format(); }
	const NodeName& node() const { return context_->node(); }

	/// The applied answer, keeping the node's code: the commit point comes once every replacement is ready.
	Changed applied() { return Changed::erased(std::nullopt); }
	/// The applied answer with a new code, swapped in at the commit point between two cycles (a discontinuity unless the
	/// type carries its state into it). By value, as on creation; it takes no loop's state, which lives across a change.
	template <class Code> Changed applied(Code code) {
		static_assert(SpecCode<Code> || std::same_as<Code, PassThroughPairs>,
		              "configure answers with its code by value: a struct with process, or passThrough(...)");
		if constexpr (std::same_as<Code, PassThroughPairs>) {
			if (code.pairs().empty()) throw std::logic_error("applied given a pass-through that was moved from");
		}
		NodeCode erased(std::move(code));
		if constexpr (SpecCode<Code>) {
			static_assert(static_cast<const void*>(&spec_detail::ProcessOf<decltype(&Code::process)>::spec) ==
			                  static_cast<const void*>(&Spec),
			              "configure answers with a code written against its own type's spec");
			static_cast<spec_detail::BankBoundCode&>(*erased.own()).bind(context_->ports(), context_->realtime());
		} else {
			static_assert(!spec_detail::needsCode<Spec>,
			              "a spec with writer::rt fields, log codes or triggers answers with "
			              "a code of its own, never a pass-through");
		}
		return Changed::erased(std::move(erased));
	}

	/// As CreateOf::post, staged until the commit point; destroyed unrun if the change is superseded or falls back.
	template <class Handle, class Work> void post(Handle, Work&& work) {
		static_assert(ownLoop<Handle>, "a context takes only its spec's loops");
		context_->postToLoop(LoopsOf<Spec>::template indexOf<Handle>,
		                     spec_detail::loopWork<Handle>(std::forward<Work>(work)));
	}
	/// A new RTP receiver for the one on this inside input bank, behind a hold the framework takes on the outputs the
	/// bank feeds: made before break on a new port, break-first on the same one
	/// (docs/writing-a-node-type.md#make-before-break).
	template <class Bank> void attach(Bank, RtpReceiverShape shape) {
		checkCompanion<Bank, RtpReceiverShape>();
		context_->attachEngine(Bank::name, std::move(shape), Order::makeFirst);
	}
	/// A new sound card for the one on this inside bank, behind a hold the framework takes on what the bank carries;
	/// made before break, or break-first when `order` says so (one ALSA device opened twice).
	template <class Bank> void attach(Bank, SoundCardShape shape, Order order = Order::makeFirst) {
		checkCompanion<Bank, SoundCardShape>();
		context_->attachEngine(Bank::name, std::move(shape), order);
	}
	/// A new source for the one on this inside input bank (docs/writing-a-node-type.md#sinks-and-sources-in-a-change):
	/// its writer valid at once, played by the block from the commit's invoke on, when the old one's writer is closed.
	template <class Bank> SourceWriter& attach(Bank, SourceShape shape, std::unique_ptr<SourceClock> clock = nullptr) {
		checkCompanion<Bank, SourceShape>();
		return context_->source(Bank::name, shape, std::move(clock));
	}
	/// A new sink for the one on this inside output bank, with its own consumer, in creation's three forms: begun before
	/// the commit point, tapped from the commit's invoke on, the old one draining to its own consumer after it.
	template <class Bank, std::same_as<SinkShape> Shape>
	SinkControl& attach(Bank, Shape shape, std::unique_ptr<SinkConsumer> consumer) {
		checkCompanion<Bank, SinkShape>();
		return context_->sink(Bank::name, shape, std::move(consumer));
	}
	template <class Bank, std::same_as<SinkShape> Shape> void attach(Bank, Shape) {
		static_assert(sizeof(Bank) == 0, "a sink is given its one consumer: cx.attach(bank, shape, consumer)");
	}
	template <class Bank, class Handle, class State, class Member>
		requires isLoopHandle<Handle>
	SinkControl& attach(Bank, SinkShape shape, Handle, Member State::* member) {
		checkCompanion<Bank, SinkShape>();
		static_assert(ownLoop<Handle>, "a sink is consumed by one of its spec's loops");
		static_assert(Handle::stateful && std::derived_from<typename Handle::State, State>,
		              "a sink's consumer is a member of that loop's state");
		static_assert(std::derived_from<Member, SinkConsumer>, "a sink's consumer is a SinkConsumer");
		if constexpr (Handle::stateful && std::derived_from<typename Handle::State, State> &&
		              std::derived_from<Member, SinkConsumer>) {
			using Own = typename Handle::State;
			return context_->sink(
				Bank::name, shape,
				StateSinkConsumer{LoopsOf<Spec>::template indexOf<Handle>, [member](void* state) -> SinkConsumer& {
									  return static_cast<Own*>(state)->*member;
								  }});
		}
	}
	template <class Bank, class Handle>
		requires isLoopHandle<Handle>
	SinkControl& attach(Bank, SinkShape shape, Handle) {
		checkCompanion<Bank, SinkShape>();
		static_assert(ownLoop<Handle>, "a sink is consumed by one of its spec's loops");
		using State = typename Handle::State;
		static_assert(Handle::stateful && std::derived_from<State, SinkConsumer>,
		              "a loop's state that consumes a sink is a SinkConsumer");
		return context_->sink(
			Bank::name, shape,
			StateSinkConsumer{LoopsOf<Spec>::template indexOf<Handle>,
		                      [](void* state) -> SinkConsumer& { return *static_cast<State*>(state); }});
	}
	/// A pipe receiver is never replaced in a change (its FIFO is its program's), nor is anything that is no companion.
	template <class Bank, class Shape, class... Rest>
		requires(!CompanionShape<std::remove_cvref_t<Shape>> ||
	             std::same_as<std::remove_cvref_t<Shape>, PipeReceiverShape>)
	void attach(Bank, Shape&&, Rest&&...) {
		static_assert(sizeof(Bank) == 0, "a change attaches a replacement for a sound card, an RTP receiver, a source "
		                                 "or a sink");
	}
	/// A new recording for `replaced`, one the configuration holds (docs/writing-a-node-type.md#recordings-in-a-change):
	/// claimed and opened beside it before the change's companions; the old one is closed once the commit point
	/// completed and every sink the change replaced finished, so a consumer may write it until then.
	Recording& recording(const Recording& replaced, RecordingRequest request) {
		return context_->recording(replaced, request);
	}
	/// A port the type's own thread binds once the change is made (docs/writing-a-node-type.md#claims-in-a-change):
	/// judged when this call returns, before any other new acquisition; a refusal falls the change back
	/// (resource-refused). A port the configuration already holds is its own, granted unprobed.
	void claim(claim::TcpListen port) { context_->claim(claim::of(port)); }
	void claim(claim::Udp port) { context_->claim(claim::of(port)); }
	/// A port the configuration holds and the change no longer needs: released at the commit point, after everything
	/// the change replaced, so another node can take it only once this one runs without it.
	void unclaim(claim::TcpListen port) { context_->unclaim(claim::of(port)); }
	void unclaim(claim::Udp port) { context_->unclaim(claim::of(port)); }
	/// The new configuration's latency on a path, as CreateOf::declareLatency, carried from the commit point.
	template <class From, class To> void declareLatency(From, To, Latency latency) {
		static_assert(ownBank<From> && ownBank<To>, "declareLatency takes its spec's banks");
		static_assert(From::direction == Direction::input && To::direction == Direction::output,
		              "declareLatency names a pair: an input bank, then an output bank");
		context_->declareLatency(From::name, To::name, latency);
	}
	/// The change's one later answer, and what configure returns (docs/writing-a-node-type.md#whenready-and-configuredone):
	/// once every acquisition this call made is ready (claims granted and probed, replacement companions running and
	/// linked) and every hold acknowledged, `work(state, done)` is posted to loop `Handle` as the node's own work (a loop
	/// with no state: `work(done)`). If the change ends before that, `work` is destroyed unrun on the main loop; once
	/// posted it runs, even if a stop, a restart or a removal began after the post. The commit point (staged posts
	/// released, holds faded in, replaced resources released) comes after its done(). The code stays the node's.
	template <class Handle, class Work> Changed whenReady(Handle, Work work) {
		static_assert(ownLoop<Handle>, "a context takes only its spec's loops");
		if constexpr (Handle::stateful)
			static_assert(std::invocable<Work&, typename Handle::State&, ConfigureDone>,
			              "whenReady's work takes the loop's state and the change's ConfigureDone");
		else static_assert(std::invocable<Work&, ConfigureDone>, "whenReady's work takes the change's ConfigureDone");
		context_->whenReady(LoopsOf<Spec>::template indexOf<Handle>,
		                    [work = std::move(work)](void* state, ConfigureDone done) mutable {
								if constexpr (Handle::stateful)
									work(*static_cast<typename Handle::State*>(state), std::move(done));
								else work(std::move(done));
							});
		return Changed::continued();
	}
	/// Holds these routable output banks silent: from the next cycle each fades to zero over 5 ms and stays there until
	/// the commit point, which fades it back in (or until release).
	template <class... Banks> void hold(Banks...) {
		static_assert(sizeof...(Banks) > 0, "hold names the banks it holds");
		static_assert((routableOutput<Banks> && ...), "hold names a routable output bank of its spec");
		const std::array<std::string_view, sizeof...(Banks)> names{Banks::name...};
		context_->hold(names);
	}
	/// Holds every routable output bank.
	void holdAll() { context_->holdAll(); }
	/// Releases a hold before the commit point: the banks fade back in.
	template <class... Banks> void release(Banks...) {
		static_assert(sizeof...(Banks) > 0, "release names the banks it releases");
		static_assert((routableOutput<Banks> && ...), "release names a routable output bank of its spec");
		const std::array<std::string_view, sizeof...(Banks)> names{Banks::name...};
		context_->release(names);
	}

private:
	template <const auto&, class> friend class spec_detail::SpecInstance;
	explicit ChangeOf(ChangeContext& context) noexcept : context_(&context) {}
	template <class Bank, class Shape> static constexpr void checkCompanion() noexcept {
		static_assert(ownBank<Bank>, "a context takes only its spec's banks");
		CompanionBank<Shape>::template check<Bank>();
	}
	ChangeContext* context_;
};

// ---- The erased instance ----

/// One configuration's instance, as NodeHost drives it (docs/writing-a-node-type.md#rules-the-framework-enforces): made
/// by NodeConfig::make at T2, freed at teardown step 5 after the code. Main thread for every call. A type's exception
/// passes through every call: NodeHost catches it at the call and fails the node (`<hook> threw: <what>`, its code by
/// fact: failureCodeFor).
class ErasedInstance {
public:
	virtual ~ErasedInstance() = default;
	/// The spec's loops, in declaration order (docs/writing-a-node-type.md#loops).
	virtual std::span<const LoopDeclaration> loops() const noexcept { return {}; }
	/// The spec's stores, in declaration order (docs/writing-a-node-type.md#stores).
	virtual std::span<const StoreDeclaration> stores() const noexcept { return {}; }
	/// T2: creation's configure, through `cx` (valid only during the call; SpecInstance hands it to the type as
	/// CreateOf<Spec>). Called once, timed as the hook `configure`.
	virtual Configured configure(CreateContext& cx) = 0;
	/// T5 and T8, once the drive completed; timed as `start`.
	virtual void start() {}
	/// Once after every start() that returned (docs/writing-a-node-type.md#the-class pairing); timed as `stop`.
	virtual void stop() {}
	/// Whether the instance has a change version (docs/writing-a-node-type.md#the-change-version): without one, every
	/// change restarts the node.
	virtual bool changes() const noexcept { return false; }
	/// A change in place to `now` (its config outlives the instance until the change ends), through `cx` (valid only
	/// during the call); timed as the hook `configure`. The instance's settings stay the applied ones until adopt().
	virtual Changed change(ChangeContext& cx, const NodeConfig& now) {
		static_cast<void>(cx);
		static_cast<void>(now);
		return Changed::restart("the type has no change version");
	}
	/// The commit point: `now`'s settings are the instance's from here (`now` outlives the instance).
	virtual void adopt(const NodeConfig& now) { static_cast<void>(now); }
	/// How long a change in place may take before it falls back to a restart (the spec's `.lifecycle`).
	virtual std::chrono::milliseconds reconfigureDeadline() const noexcept { return Lifecycle{}.reconfigureDeadline; }

protected:
	/// The applied answer as the erased layer makes it: an instance written by hand (the framework's own tests) answers
	/// with it, and a type's class with `cx.applied(...)`, which is held to its spec. The code, and one state for each
	/// loop the instance declares with one, by index. NodeHost holds what it is given to the instance's loops: a null
	/// code, a state missing, given twice or for a loop declared without one is the instance's maker's bug, told as
	/// ear's own (docs/architecture.md#faults-by-owner). The framework still waits for every resource the configure
	/// asked for.
	template <std::same_as<ErasedLoopState>... States> static Configured applied(NodeCode code, States... states) {
		return Configured::erased(std::move(code), std::move(states)...);
	}
	/// A change's applied answer as the erased layer makes it, for an instance written by hand: keeping the code, or
	/// with a new one.
	static Changed changed() { return Changed::erased(std::nullopt); }
	static Changed changed(NodeCode code) { return Changed::erased(std::move(code)); }
};

namespace spec_detail {
/// A class's start or stop: found by name, then exactly void() (noexcept allowed).
template <class Member, class Type> inline constexpr bool exactlyVoid =
	std::same_as<Member, void (Type::*)()> || std::same_as<Member, void (Type::*)() noexcept>;

/// Whether a class declares a member of a name at all, whatever its access, overloads or templates: a probe deriving from
/// it and from a struct that declares the name makes the lookup ambiguous only when the class has one too. So a private,
/// overloaded or template start, stop, check or change version is found, then refused, never silently skipped.
struct StartName {
	void start();
};
struct StopName {
	void stop();
};
struct CheckName {
	void check();
};
template <class Type, class Name> struct NameProbe : Type, Name {};
template <class Type> inline constexpr bool declaresStart = !requires { &NameProbe<Type, StartName>::start; };
template <class Type> inline constexpr bool declaresStop = !requires { &NameProbe<Type, StopName>::stop; };
template <class Type> inline constexpr bool declaresCheck = !requires { &NameProbe<Type, CheckName>::check; };

/// Converts to any reference: a call with it finds any configure of three parameters, whatever its first.
struct AnyContext {
	template <class T> operator T&() const; // NOLINT: implicit on purpose; never defined, only named in requires
};

/// The instance makeNodeType<Spec, Type>() makes for each configuration
/// (docs/writing-a-node-type.md#rules-the-framework-enforces). It holds NodeOf, then the type's object (constructed
/// from NodeOf or from nothing), and calls its hooks. `settings` are the config's, which outlives the instance
/// (teardown step 5).
template <const auto& Spec, class Type> class SpecInstance final : public ErasedInstance {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
	using Checked = CheckedOf<Spec>;
	static_assert(std::is_class_v<Type> && !std::is_final_v<Type>,
	              "a type's class is a class, not final (the framework finds its members by name)");
	static_assert(
		requires(const Checked& settings) {
			{ Type::ports(settings) } -> std::same_as<Members<Spec>>;
		}, "a type's class has static Members<spec> ports(const CheckedOf<spec>&)");
	static_assert(std::constructible_from<Type, NodeOf<Spec>&> || std::default_initializable<Type>,
	              "a type's class is constructed from NodeOf<spec>& or from nothing");
	static_assert(
		requires { static_cast<Configured (Type::*)(CreateOf<Spec>&, const Checked&)>(&Type::configure); },
		"a type's class has Configured configure(CreateOf<spec>&, const CheckedOf<spec>&)");
	// The change version: optional; once any configure of three parameters is callable (with a ChangeOf, or with
	// any context), its signature is exactly this.
	static constexpr bool hasChange = requires(Type& type, ChangeOf<Spec>& change, const Checked& settings) {
		type.configure(change, settings, settings);
	} || requires(Type& type, const Checked& settings) { type.configure(AnyContext{}, settings, settings); };
	static consteval bool changesExactly() {
		if constexpr (hasChange)
			return requires {
				static_cast<Changed (Type::*)(ChangeOf<Spec>&, const Checked&, const Checked&)>(&Type::configure);
			};
		else return true;
	}
	static_assert(changesExactly(), "a type's change version is Changed configure(ChangeOf<spec>&, const "
	                                "CheckedOf<spec>&, const CheckedOf<spec>&)");
	// start and stop: found by name whatever their access or overloads, then public, single and exactly void().
	static constexpr bool hasStart = declaresStart<Type>;
	static constexpr bool hasStop = declaresStop<Type>;
	static_assert(!hasStart || requires { &Type::start; }, "a type's start is public and not overloaded");
	static_assert(!hasStop || requires { &Type::stop; }, "a type's stop is public and not overloaded");
	static consteval bool startsExactly() {
		if constexpr (requires { &Type::start; }) return exactlyVoid<decltype(&Type::start), Type>;
		else return true;
	}
	static consteval bool stopsExactly() {
		if constexpr (requires { &Type::stop; }) return exactlyVoid<decltype(&Type::stop), Type>;
		else return true;
	}
	static_assert(startsExactly(), "a type's start is void start()");
	static_assert(stopsExactly(), "a type's stop is void stop()");

public:
	SpecInstance(NodeContext& node, const Checked& settings) : node_(node), settings_(&settings), type_(make(node_)) {}
	std::span<const LoopDeclaration> loops() const noexcept override { return LoopsOf<Spec>::declared; }
	std::span<const StoreDeclaration> stores() const noexcept override { return StoresOf<Spec>::declared; }
	Configured configure(CreateContext& cx) override {
		CreateOf<Spec> create(cx);
		return type_.configure(create, *settings_);
	}
	bool changes() const noexcept override { return hasChange; }
	std::chrono::milliseconds reconfigureDeadline() const noexcept override {
		return Spec.lifecycle.reconfigureDeadline;
	}
	Changed change(ChangeContext& cx, const NodeConfig& now) override;
	void adopt(const NodeConfig& now) override;
	void start() override {
		if constexpr (hasStart) type_.start();
	}
	void stop() override {
		if constexpr (hasStop) type_.stop();
	}

private:
	static Type make(NodeOf<Spec>& node) {
		if constexpr (std::constructible_from<Type, NodeOf<Spec>&>) return Type(node);
		else return Type();
	}
	NodeOf<Spec> node_;
	const Checked* settings_; // the applied settings' config's, re-pointed at a change's commit point (adopt)
	Type type_;               // after node_, which it may keep
};

/// The config makeNodeType's type makes (docs/writing-a-node-type.md#the-class): the checked settings and the ports,
/// and an instance of the class for each configuration. It has no start: NodeConfig's default throws, and nothing calls
/// it.
template <const auto& Spec, class Type> class SpecConfig final : public NodeConfig {
public:
	explicit SpecConfig(const CheckedOf<Spec>& settings)
		: settings_(settings), ports_(Type::ports(settings_).ports()) {}
	const Ports& ports() const override { return ports_; }
	std::unique_ptr<ErasedInstance> make(NodeContext& node) const override {
		return std::make_unique<SpecInstance<Spec, Type>>(node, settings_);
	}
	const CheckedOf<Spec>& settings() const noexcept { return settings_; }
	std::span<const StatusDeclaration* const> statusDeclared() const override { return StatusOf<Spec>::declared; }
	std::span<const EventDeclaration> eventsDeclared() const override { return EventsOf<Spec>::declared; }
	std::span<const LogCodeDeclaration> logCodesDeclared() const override { return LogCodesOf<Spec>::declared; }
	std::span<const ActionDeclaration> actionsDeclared() const override { return ActionsOf<Spec>::declared; }
	std::expected<void, std::string> runCall(std::size_t action, void* state, CheckedSettings&& args) const override {
		const spec_detail::CallRunner run = spec_detail::CallRunners<ActionsOf<Spec>>::runners.at(action);
		if (run == nullptr) return NodeConfig::runCall(action, state, std::move(args));
		return run(state, std::move(args));
	}

private:
	CheckedOf<Spec> settings_;
	Ports ports_;
};

/// A change's `now` is always a config of the same type (the plan changes a node in place only within its type), so
/// it is this type's SpecConfig.
template <const auto& Spec, class Type>
Changed SpecInstance<Spec, Type>::change(ChangeContext& cx, const NodeConfig& now) {
	if constexpr (hasChange) {
		ChangeOf<Spec> change(cx);
		return type_.configure(change, *settings_, static_cast<const SpecConfig<Spec, Type>&>(now).settings());
	} else {
		return ErasedInstance::change(cx, now);
	}
}
template <const auto& Spec, class Type> void SpecInstance<Spec, Type>::adopt(const NodeConfig& now) {
	settings_ = &static_cast<const SpecConfig<Spec, Type>&>(now).settings();
}

/// The type makeNodeType makes. It hands NodeType the spec's table at run time, never as a template argument
/// (`Spec.settings`): GCC 14 gives a template argument that names a subobject (or a variable template's
/// specialization) external linkage whatever its object's, so two types' `spec.settings`, each in its own file's
/// unnamed namespace, would become one symbol and the linker keep one type's table for both.
template <const auto& Spec, class Type> class SpecNodeType final : public NodeType {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
#ifdef EAR_NODE_TYPE_NAME // ear_node_type() (cmake/NodeType.cmake) gives a type's library its folder's name
	static_assert(Spec.settings.name() == EAR_NODE_TYPE_NAME, "a type's table is named for its folder");
#endif
	using Checked = CheckedOf<Spec>;
	static_assert(sizeof(SpecInstance<Spec, Type>) > 0); // the class's checks, at makeNodeType
	static_assert(Spec.lifecycle.reconfigureDeadline >= reconfigureDeadlineLeast &&
	                  Spec.lifecycle.reconfigureDeadline <= reconfigureDeadlineMost,
	              "a type's reconfigureDeadline is 1 s to 60 s");

public:
	SpecNodeType() noexcept
		: NodeType(Spec.settings, StoresOf<Spec>::declared, Spec.lifecycle.reconfigureDeadline, BanksOf<Spec>::declared,
	               StatusOf<Spec>::declared, EventsOf<Spec>::declared, ActionsOf<Spec>::declared) {}

private:
	/// The table's walk passed: the class's check, a rule across settings, then the config.
	MadeConfig makeConfig(const NodeName&, CheckedSettings walked) const override {
		const Checked settings(std::move(walked));
		// Found by name whatever its access or overloads, then held to public, single and its exact signature, so a
		// check that is private or takes its settings by another type is refused rather than never run.
		if constexpr (declaresCheck<Type>) {
			static_assert(requires { &Type::check; }, "a type's check is public and not overloaded");
			if constexpr (requires { &Type::check; }) {
				static_assert(
					std::same_as<decltype(&Type::check), std::expected<void, std::string> (*)(const Checked&)>,
					"a type's check is static std::expected<void, std::string> check(const CheckedOf<spec>&)");
				if (auto checked = Type::check(settings); !checked) return std::unexpected(std::move(checked.error()));
			}
		}
		return std::make_unique<const SpecConfig<Spec, Type>>(settings);
	}
};
} // namespace spec_detail

/// The type the class `Type` gives `Spec`, called from the type's one named function. Its name, setting kinds and checks
/// are the spec's table's, and (in a type's own library) the table must be named for the type's folder.
template <const auto& Spec, class Type> std::unique_ptr<const NodeType> makeNodeType() {
	return std::make_unique<const spec_detail::SpecNodeType<Spec, Type>>();
}

} // namespace ear
