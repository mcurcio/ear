// What a node type's class is handed and answers (docs/design/node-framework.md 6.4 and 6.10, the type author's API;
// docs/design/node-lifecycle-modules.md section 3, the home of these declarations, step L-4a of slice 3a), and the
// erased instance NodeHost drives (docs/design/node-lifecycle.md 5.9 and 5.10).
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
// the node (node-lifecycle-modules.md D-M2, the owner's answer to Q1).
//
// Every call here is on the main loop. CreateOf is valid only inside the configure it is handed to (a call after it
// returned is a std::logic_error naming the call, "<call> outside configure", thrown by NodeHost); NodeOf is valid for
// the instance's life. A rule the type breaks at a call or at configure's return fails the node start-failed with the
// rule's text, never escaping NodeHost (D-M3). The bodies in this header are forwarding and compile-time checks only;
// NodeHost implements the erased contexts. SpecConfig, SpecNodeType and makeNodeType, at the end, make a type of the
// class (node-lifecycle-modules.md 3.6, D-M1).
#pragma once
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include "ear/core/node_type.hpp"
#include "ear/core/sink.hpp"
#include "ear/core/type_spec.hpp"
namespace ear {

// ---- Answers ----

/// What creation's configure answers (node-framework.md 6.4). It has no restart: that is the change version's answer.
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

/// What the change version of configure answers (node-framework.md 6.4 and 6.10; slice 3d). Declared in 3a only so that
/// neither answer type can stand for the other; slice 3d defines the rest (and a later answer, made by whenReady).
class Changed {
public:
	static Changed applied() noexcept;
	static Changed restart(std::string reason);

private:
	Changed() = default;
};

// ---- The erased contexts NodeHost implements ----

/// What NodeOf<Spec> wraps: the node's own, for one configuration's instance. NodeHost implements it on the
/// configuration, declared before the instance, so it outlives the instance. Main thread, but handle(), whose result
/// any of the node's threads may keep and call until the configuration's teardown ended (InsidesEvents' rules).
class NodeContext {
public:
	virtual AudioFormat format() const noexcept = 0;
	virtual const NodeName& node() const noexcept = 0;
	/// 3a: the configuration's Channel (say, lost; I6); slice 3c makes it NodeHandle&.
	virtual InsidesEvents& handle() noexcept = 0;

protected:
	~NodeContext() = default;
};

/// What `cx.sink(...)` returns: names the sink's one consumer. Main thread, inside configure only.
class SinkRequest {
public:
	/// A consumer of the type's own, given whole and owned by the sink (file's WavWriter). Exactly once per sink, and
	/// not null: a second call, a null consumer or none by configure's return (when it answered applied) is a
	/// std::logic_error naming the node and the sink's bank. Slice 3c adds the loop-member forms (node-framework.md
	/// 6.10) as templates over this.
	virtual void consumedBy(std::unique_ptr<SinkConsumer> consumer) = 0;

protected:
	~SinkRequest() = default;
};

/// What creation's configure acquires through: the erased side of CreateOf<Spec>. NodeHost implements it on the node,
/// valid only inside the configure it is handed to; any call outside it throws std::logic_error. Banks arrive by name,
/// already checked against the spec at compile time; NodeHost checks only what a value decides (a sound card's
/// direction against its bank's).
class CreateContext {
public:
	virtual AudioFormat format() const = 0;
	virtual const NodeName& node() const = 0;
	/// The node's ports (its config's), to bind a code written against the spec.
	virtual const Ports& ports() const = 0;
	/// The code (prepared at T3, driven at T5). Exactly once: a second call is a std::logic_error at the call; none by an
	/// applied configure's return, one at the return.
	virtual void use(NodeCode code) = 0;
	/// Slice 3a's interim sound card (node-framework.md 6.10, "The 3a interim"): `request.bank` is filled from `bank`
	/// (a non-empty, different one is a std::logic_error), its direction must agree with the bank's (playback: an
	/// inside output; capture: an inside input; else std::logic_error); a playback card's bank is faded with the public
	/// outputs (D-L16). Made once the block is ready, in the turn's flush; `<node>.playback` or `<node>.capture`.
	virtual void soundCard(std::string_view bank, SoundCardRequest request) = 0;
	/// Slice 3a's interim RTP receiver on an inside input bank, `<node>.rtp`, likewise; never faded.
	virtual void rtpReceiver(std::string_view bank, RtpReceiverRequest request) = 0;
	/// Slice 3a's interim recording: opened at once, beneath the recordings folder; its descriptor is closed after the
	/// configuration's code and instance (I5 step 5). The error is why not (it names no path).
	virtual std::expected<int, std::string> openRecording(const RecordingRequest& request) = 0;
	/// Slice 3a's interim sink on an inside output bank (node-lifecycle.md 5.11): made at once (its thread starts); its
	/// consumer begins once configure answered applied (readiness 4').
	virtual SinkRequest& sink(std::string_view bank, SinkShape shape) = 0;
	/// The type's own code's latency (node-framework.md 9.4, D-L17): `latency` on the pair's output bank `to`, in the
	/// configuration's LatencyPlan::internal. At most once per output bank (a second is a std::logic_error).
	virtual void declareLatency(std::string_view from, std::string_view to, Latency latency) = 0;

protected:
	~CreateContext() = default;
};

// ---- The type's views of them ----

namespace spec_detail {
template <const auto& Spec, class Type> class SpecInstance;
}

/// What the instance keeps: the node's own, for the instance's life, main loop only (handle() excepted, as
/// NodeContext's). Slice 3c adds post(), slice 5 report() (node-framework.md 6.10).
template <const auto& Spec> class NodeOf {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");

public:
	NodeOf(const NodeOf&) = delete;
	NodeOf& operator=(const NodeOf&) = delete;
	AudioFormat format() const noexcept { return context_->format(); }
	const NodeName& node() const noexcept { return context_->node(); }
	/// 3a: the configuration's InsidesEvents (say, lost); keep it in a sink's consumer or a thread the instance owns.
	InsidesEvents& handle() noexcept { return context_->handle(); }

private:
	template <const auto&, class> friend class spec_detail::SpecInstance;
	explicit NodeOf(NodeContext& context) noexcept : context_(&context) {}
	NodeContext* context_;
};

/// configure on creation (T2): acquire and construct (node-framework.md 6.10; slice 3a's members, its interim calls
/// among them). Only the spec's banks, of the right direction and reach, compile. It has no holds, whenReady or
/// unclaim (the change version's), so a call the mode does not offer does not compile.
template <const auto& Spec> class CreateOf {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
	using List = BanksOf<Spec>;
	template <class Bank> static constexpr bool ownBank = isBankHandle<Bank> && List::template has<Bank>;

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
				throw std::logic_error("a type's configure gave a code written against another type's spec");
			bound->bindPorts(context_->ports());
		}
		context_->use(std::move(code));
	}

	/// 3a interim (3b: soundCard(Bank, SoundCardShape)). `request.bank` is left empty: the handle names it.
	template <class Bank> void soundCard(Bank, SoundCardRequest request) {
		static_assert(ownBank<Bank>, "a context takes only its spec's banks");
		static_assert(Bank::reach == PortBank::Reach::inside, "a sound card joins an inside bank of the spec");
		context_->soundCard(Bank::name, std::move(request));
	}
	/// 3a interim (3b: rtpReceiver(Bank, RtpReceiverShape)). `request.bank` is left empty: the handle names it.
	template <class Bank> void rtpReceiver(Bank, RtpReceiverRequest request) {
		static_assert(ownBank<Bank>, "a context takes only its spec's banks");
		static_assert(Bank::reach == PortBank::Reach::inside && Bank::direction == Direction::input,
		              "an RTP receiver joins an inside input bank of the spec");
		context_->rtpReceiver(Bank::name, std::move(request));
	}
	/// 3a interim (3c: recording()). A refusal is the caller's to answer: Configured::failed("cannot open ...").
	std::expected<int, std::string> openRecording(const RecordingRequest& request) {
		return context_->openRecording(request);
	}
	/// 3a interim realtime side (3b: a ring companion), the same call.
	template <class Bank> SinkRequest& sink(Bank, SinkShape shape) {
		static_assert(ownBank<Bank>, "a context takes only its spec's banks");
		static_assert(Bank::reach == PortBank::Reach::inside && Bank::direction == Direction::output,
		              "a sink reads an inside output bank of the spec");
		return context_->sink(Bank::name, shape);
	}
	/// The type's own code's latency, from an input bank to an output bank (it lands on `To`).
	template <class From, class To> void declareLatency(From, To, Latency latency) {
		static_assert(ownBank<From> && ownBank<To>, "declareLatency takes its spec's banks");
		static_assert(From::direction == Direction::input && To::direction == Direction::output,
		              "declareLatency names a pair: an input bank, then an output bank");
		context_->declareLatency(From::name, To::name, latency);
	}

private:
	template <const auto&, class> friend class spec_detail::SpecInstance;
	explicit CreateOf(CreateContext& context) noexcept : context_(&context) {}
	CreateContext* context_;
};

/// configure for a change (T9; node-framework.md 6.10). Slice 3d defines it. In 3a a class may write the change
/// version (its signature is checked), and it is never called: every change restarts the node
/// (node-lifecycle-modules.md D-M2, the owner's answer to Q1).
template <const auto& Spec> class ChangeOf;

// ---- The erased instance ----

/// Both contexts NodeHost implements on one set of internals, for the one call that needs either (node-lifecycle.md
/// 5.9). Valid only during configure.
struct InstanceContexts {
	CreateContext& create; // what SpecInstance hands the type, as CreateOf<Spec>
	StartContext& legacy;  // what LegacyInstance hands NodeConfig::start (5.10); interim until slice 3c
};

/// One configuration's instance, as NodeHost drives it (node-lifecycle.md 5.9): made by NodeConfig::make at T2, freed
/// at I5 step 5 after the code. Main thread for every call. A type's exception passes through every call: NodeHost
/// catches it at the call and fails the node (`<hook> threw: <what>`, 4.2's code by fact).
class ErasedInstance {
public:
	virtual ~ErasedInstance() = default;
	/// T2: creation's configure (or the legacy start). Called once, timed as the hook `configure`.
	virtual Configured configure(InstanceContexts& cx) = 0;
	/// Legacy only: whether its start returned a recipe (D-L40's check, with the outside nodes it asked for).
	virtual bool hasRecipe() const noexcept { return false; }
	/// Legacy only, T3 (every resource ready, before the machine hears it): the recipe builds the run object; its
	/// failure is the fault `start-failed`.
	virtual std::expected<void, FailureReason> ready(Ready& ready) {
		static_cast<void>(ready);
		return {};
	}
	/// T5 and T8, once the drive completed; timed as `start`.
	virtual void start() {}
	/// Once after every start() that returned (node-framework.md 6.4's pairing); timed as `stop`.
	virtual void stop() {}
	/// I5 step 4. Legacy only: RunObject::close (off the main loop for a run with threads); the new SPI has nothing to
	/// close until slice 3c's loops. `closed` from any thread, once.
	virtual void close(std::move_only_function<void()> closed) { closed(); }
};

namespace spec_detail {
/// A class's start or stop: found by name, then exactly void() (noexcept allowed).
template <class Member, class Type> inline constexpr bool exactlyVoid =
	std::same_as<Member, void (Type::*)()> || std::same_as<Member, void (Type::*)() noexcept>;

/// The instance makeNodeType<Spec, Type>() makes for each configuration (node-lifecycle.md 5.9). It holds NodeOf, then
/// the type's object (constructed from NodeOf or from nothing), and calls its hooks. `settings` are the config's, which
/// outlives the instance (I5 step 5).
template <const auto& Spec, class Type> class SpecInstance final : public ErasedInstance {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
	using Checked = CheckedOf<Spec>;
	static_assert(
		requires(const Checked& settings) {
			{ Type::ports(settings) } -> std::same_as<Members<Spec>>;
		}, "a type's class has static Members<spec> ports(const CheckedOf<spec>&)");
	static_assert(
		!requires(StartContext& context, const Checked& settings) { Type::start(context, settings); },
		"a type on makeNodeType writes configure, not a static start (node-lifecycle.md D-L13)");
	static_assert(std::constructible_from<Type, NodeOf<Spec>&> || std::default_initializable<Type>,
	              "a type's class is constructed from NodeOf<spec>& or from nothing");
	static_assert(
		requires { static_cast<Configured (Type::*)(CreateOf<Spec>&, const Checked&)>(&Type::configure); },
		"a type's class has Configured configure(CreateOf<spec>&, const CheckedOf<spec>&)");
	// The change version: optional, never called in 3a (D-M2); once it is callable, its signature is exactly this.
	static constexpr bool hasChange = requires(Type& type, ChangeOf<Spec>& change, const Checked& settings) {
		type.configure(change, settings, settings);
	};
	static consteval bool changesExactly() {
		if constexpr (hasChange)
			return requires {
				static_cast<Changed (Type::*)(ChangeOf<Spec>&, const Checked&, const Checked&)>(&Type::configure);
			};
		else return true;
	}
	static_assert(changesExactly(), "a type's change version is Changed configure(ChangeOf<spec>&, const "
	                                "CheckedOf<spec>&, const CheckedOf<spec>&)");
	static constexpr bool hasStart = requires { &Type::start; };
	static constexpr bool hasStop = requires { &Type::stop; };
	static consteval bool startsExactly() {
		if constexpr (hasStart) return exactlyVoid<decltype(&Type::start), Type>;
		else return true;
	}
	static consteval bool stopsExactly() {
		if constexpr (hasStop) return exactlyVoid<decltype(&Type::stop), Type>;
		else return true;
	}
	static_assert(startsExactly(), "a type's start is void start()");
	static_assert(stopsExactly(), "a type's stop is void stop()");

public:
	SpecInstance(NodeContext& node, const Checked& settings) : node_(node), settings_(settings), type_(make(node_)) {}
	Configured configure(InstanceContexts& cx) override {
		CreateOf<Spec> create(cx.create);
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

/// The config makeNodeType's type makes (node-lifecycle-modules.md 3.6): the checked settings and the ports, and an
/// instance of the class for each configuration. It has no start: NodeConfig's default throws, and nothing calls it.
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

/// The type makeNodeType makes. It derives from NodeType itself, handing it the spec's table at run time, not from
/// NodeTypeOf<Spec.settings>: GCC 14 gives a template argument that names a subobject (or a variable template's
/// specialization) external linkage whatever its object's, so two types' `spec.settings`, each in its own file's
/// unnamed namespace, became one symbol and the linker kept one type's table for both.
template <const auto& Spec, class Type> class SpecNodeType final : public NodeType {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
#ifdef EAR_NODE_TYPE_NAME // ear_node_type() (cmake/NodeType.cmake) gives a type's library its folder's name
	static_assert(Spec.settings.name() == EAR_NODE_TYPE_NAME, "a type's table is named for its folder");
#endif
	using Checked = CheckedOf<Spec>;
	static_assert(sizeof(SpecInstance<Spec, Type>) > 0); // the class's checks, at makeNodeType

public:
	SpecNodeType() noexcept : NodeType(Spec.settings) {}

private:
	/// The table's walk passed: the class's check, a rule across settings, then the config.
	MadeConfig makeConfig(const NodeName&, CheckedSettings walked) const override {
		const Checked settings(std::move(walked));
		// Found by name, then held to its exact signature, so a check taking its settings by another type is refused
		// rather than never run.
		if constexpr (requires { &Type::check; }) {
			static_assert(std::same_as<decltype(&Type::check), std::expected<void, std::string> (*)(const Checked&)>,
			              "a type's check is static std::expected<void, std::string> check(const CheckedOf<spec>&)");
			if (auto checked = Type::check(settings); !checked) return std::unexpected(std::move(checked.error()));
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
