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
//   	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
//   		cx.use(WhiteCode{});
//   		return Configured::applied();
//   	}
//   };
//   }
//
// The class's members, each found by name and held to exactly this at compile time (SpecInstance's checks):
//   static Members<spec> ports(const CheckedOf<spec>&);                                  required
//   static std::expected<void, std::string> check(const CheckedOf<spec>&);               optional, at parse
//   explicit Type(NodeOf<spec>&);  or  Type();                                           one of the two
//   Configured configure(CreateOf<spec>&, const CheckedOf<spec>&);                       required
//   Changed configure(ChangeOf<spec>&, const CheckedOf<spec>&, const CheckedOf<spec>&);  optional; slice 3d's
//   void start();  void stop();                                                          optional
//
// The change version compiles (its signature checked) but slice 3a never calls it: every change of settings restarts
// the node.
//
// Every call here is on the main loop. CreateOf is valid only inside the configure it is handed to (a call after it
// returned is a std::logic_error naming the call, "<call> outside configure", thrown by NodeHost); NodeOf is valid for
// the instance's life. A rule the type breaks at a call or at configure's return fails the node start-failed with the
// rule's text, never escaping NodeHost. The bodies in this header are forwarding and compile-time checks only;
// NodeHost implements the erased contexts. SpecConfig, SpecNodeType and makeNodeType, at the end, make a type of the
// class (docs/writing-a-node-type.md#the-class).
#pragma once
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
#include "ear/core/companions.hpp"
#include "ear/core/loop.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/program.hpp"
#include "ear/core/recording.hpp"
#include "ear/core/sink.hpp"
#include "ear/core/source.hpp"
#include "ear/core/store.hpp"
#include "ear/core/type_spec.hpp"
namespace ear {

// ---- Answers ----

/// What creation's configure answers (docs/writing-a-node-type.md#the-class). It has no restart: that is the change
/// version's answer.
class Configured {
public:
	/// The framework still waits for every resource the configure asked for (readiness 3' and 4').
	static Configured applied() noexcept { return Configured(std::nullopt); }
	/// A failure only the type can see: the node fails `start-failed: <detail>`, its configuration torn down.
	static Configured failed(std::string detail) { return Configured(std::move(detail)); }
	/// For the framework.
	bool isFailed() const noexcept { return failure_.has_value(); }
	/// For the framework; empty unless isFailed().
	const std::string& detail() const noexcept { return failure_ ? *failure_ : empty(); }

private:
	explicit Configured(std::optional<std::string> failure) noexcept : failure_(std::move(failure)) {}
	static const std::string& empty() noexcept {
		static const std::string none;
		return none;
	}
	std::optional<std::string> failure_;
};

/// What the change version of configure answers (docs/writing-a-node-type.md#the-class; slice 3d). Declared in 3a only
/// so that neither answer type can stand for the other; slice 3d defines the rest (and a later answer, made by
/// whenReady).
class Changed {
public:
	/// 3a never calls the change version, so these only let one compile and link; slice 3d gives them their meaning.
	static Changed applied() noexcept { return Changed(); }
	static Changed restart(std::string reason) {
		static_cast<void>(reason);
		return Changed();
	}

private:
	Changed() = default;
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
	/// The code (prepared at T3, driven at T5). Exactly once: a second call is a std::logic_error at the call; none by an
	/// applied configure's return, one at the return.
	virtual void use(NodeCode code) = 0;
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
	virtual void sink(std::string_view bank, SinkShape shape, SinkConsumerFrom consumer) = 0;
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
	/// Declared loop `loop`'s state (docs/writing-a-node-type.md#loops), owned from now on by the framework, which
	/// destroys it on the loop's thread at teardown step 4 (on the main loop if the loop never started). Exactly once
	/// for a loop declared with a state type, by an applied configure's return; never for one declared without.
	virtual void loopState(std::size_t loop, std::unique_ptr<void, void (*)(void*)> state) = 0;
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
	/// A rule CreateOf finds broken before it reaches a call above (a code written against another type's spec): records
	/// it on the configuration, so the node fails with it whatever configure does next, and throws std::logic_error(rule).
	[[noreturn]] virtual void refuse(std::string rule) = 0;

protected:
	~CreateContext() = default;
	/// The FIFO of the pipe receiver on `bank`: what attachEngine returns for one, and nothing else makes.
	static part::Fifo fifoOf(std::string bank) { return part::Fifo(std::move(bank)); }
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
/// NodeContext's). Slice 3c adds post(), slice 5 report() (docs/writing-a-node-type.md#the-class).
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

/// configure on creation (T2): acquire and construct (docs/writing-a-node-type.md#the-class; slice 3a's members, its
/// interim calls among them). Only the spec's banks, of the right direction and reach, compile. It has no holds,
/// whenReady or unclaim (the change version's), so a call the mode does not offer does not compile.
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

	/// The code, once. A code written against a spec is bound to the node's ports here; one written against another
	/// type's spec is a std::logic_error (it would read this node's ports by that spec's banks).
	void use(NodeCode code) {
		if (auto* bound = dynamic_cast<spec_detail::BankBoundCode*>(code.own().get())) {
			if (bound->spec() != &Spec)
				context_->refuse("a type's configure gave a code written against another type's spec");
			bound->bindPorts(context_->ports());
		}
		context_->use(std::move(code));
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
		return *context_->attachEngine(Bank::name, std::move(shape));
	}
	/// A source on an inside input bank (docs/writing-a-node-type.md#sources): what the type's ordinary thread writes,
	/// played into the bank by the block before the code; `clock` its own clock, null for a source with no clock
	/// (docs/writing-a-node-type.md#a-sources-clock).
	template <class Bank> SourceWriter& attach(Bank, SourceShape shape, std::unique_ptr<SourceClock> clock = nullptr) {
		checkCompanion<Bank, SourceShape>();
		return context_->source(Bank::name, shape, std::move(clock));
	}
	/// A sink on an inside output bank with its one consumer (docs/writing-a-node-type.md#sinks): here a consumer of
	/// the type's own, given whole and owned by the sink (file's WavWriter).
	template <class Bank> void attach(Bank, SinkShape shape, std::unique_ptr<SinkConsumer> consumer) {
		checkCompanion<Bank, SinkShape>();
		context_->sink(Bank::name, shape, std::move(consumer));
	}
	/// A sink with no consumer does not compile: nothing would ever read it.
	template <class Bank> void attach(Bank, SinkShape) {
		static_assert(sizeof(Bank) == 0, "a sink is given its one consumer: cx.attach(bank, shape, consumer)");
	}
	/// The sink's consumer is `member` of loop `Handle`'s state (`cx.attach(bank, shape, worker, &Worker::toServer)`):
	/// it lives as long as the state, which outlives the sink's last call. Its calls still run on the sink's own
	/// thread, never the loop's. A loop the spec lacks, a loop without a state, or a member that is no SinkConsumer
	/// does not compile.
	template <class Bank, class Handle, class State, class Member>
		requires isLoopHandle<Handle>
	void attach(Bank, SinkShape shape, Handle, Member State::* member) {
		checkCompanion<Bank, SinkShape>();
		static_assert(ownLoop<Handle>, "a sink is consumed by one of its spec's loops");
		static_assert(Handle::stateful && std::same_as<State, typename Handle::State>,
		              "a sink's consumer is a member of that loop's state");
		static_assert(std::derived_from<Member, SinkConsumer>, "a sink's consumer is a SinkConsumer");
		context_->sink(
			Bank::name, shape,
			StateSinkConsumer{LoopsOf<Spec>::template indexOf<Handle>,
		                      [member](void* state) -> SinkConsumer& { return static_cast<State*>(state)->*member; }});
	}
	/// The sink's consumer is loop `Handle`'s state itself.
	template <class Bank, class Handle>
		requires isLoopHandle<Handle>
	void attach(Bank, SinkShape shape, Handle) {
		checkCompanion<Bank, SinkShape>();
		static_assert(ownLoop<Handle>, "a sink is consumed by one of its spec's loops");
		using State = typename Handle::State;
		static_assert(Handle::stateful && std::derived_from<State, SinkConsumer>,
		              "a loop's state that consumes a sink is a SinkConsumer");
		context_->sink(Bank::name, shape,
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

	/// Loop `Handle`'s state (docs/writing-a-node-type.md#loops), constructed here on the main loop with no I/O and no
	/// threads; from now on the framework owns it, and the instance reaches it only by a post. Exactly once for a loop
	/// declared with a state type; a loop declared without one takes none (it does not compile).
	template <class Handle> void loop(Handle, std::unique_ptr<typename Handle::State> state) {
		static_assert(ownLoop<Handle>, "a context takes only its spec's loops");
		static_assert(Handle::stateful, "a loop declared without a state takes none");
		using State = typename Handle::State;
		context_->loopState(LoopsOf<Spec>::template indexOf<Handle>,
		                    std::unique_ptr<void, void (*)(void*)>(
								state.release(), [](void* given) { delete static_cast<State*>(given); }));
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

/// configure for a change (T9; docs/writing-a-node-type.md#the-class). Slice 3d defines it. In 3a a class may write the
/// change version (its signature is checked), and it is never called: every change restarts the node.
template <const auto& Spec> class ChangeOf;

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
	// The change version: optional, never called in 3a; once any configure of three parameters is callable (with
	// a ChangeOf, or with any context), its signature is exactly this.
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
	SpecInstance(NodeContext& node, const Checked& settings) : node_(node), settings_(settings), type_(make(node_)) {}
	std::span<const LoopDeclaration> loops() const noexcept override { return LoopsOf<Spec>::declared; }
	std::span<const StoreDeclaration> stores() const noexcept override { return StoresOf<Spec>::declared; }
	Configured configure(CreateContext& cx) override {
		CreateOf<Spec> create(cx);
		return type_.configure(create, settings_);
	}
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
	const Checked& settings_;
	Type type_; // after node_, which it may keep
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

private:
	CheckedOf<Spec> settings_;
	Ports ports_;
};

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

public:
	SpecNodeType() noexcept : NodeType(Spec.settings, StoresOf<Spec>::declared) {}

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
