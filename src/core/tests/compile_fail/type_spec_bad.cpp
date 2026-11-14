// What must not compile about a type spec, its class, the contexts its configure is handed, Io and the harness
// (src/core/CMakeLists.txt registers one CTest test per case, each with the reason it must be refused;
// docs/writing-a-node-type.md#the-class; docs/writing-a-node-type.md#rules-the-framework-enforces). With no
// case defined this is the control, and it must compile: what the cases break is only what they name.
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

// As ear_node_type() gives a type's library: a spec's table must be named for its folder.
#ifdef EAR_CASE_spec_not_named_for_folder
#define EAR_NODE_TYPE_NAME "folder"
#else
#define EAR_NODE_TYPE_NAME "t"
#endif
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace type_spec_bad {

using namespace ear;

constexpr auto in = bank::input<"in">;
constexpr auto out = bank::output<"out">;
constexpr auto insideOut = bank::insideOutput<"inside_out">;
constexpr auto insideIn = bank::insideInput<"inside_in">;
constexpr TypeSpec spec{.settings = SettingsTable<>{"t"}, .banks = Banks<in, out, insideOut, insideIn>{}};
using Checked = CheckedOf<spec>;
// Another type's spec, with banks this spec lacks: of the same direction and reach, under other names. (A bank of the
// same name, direction and reach is the same handle, and compiles: type_spec_test.cpp shows it.)
constexpr auto otherOut = bank::output<"other">;
constexpr auto otherInsideOut = bank::insideOutput<"other_inside">;
constexpr TypeSpec otherSpec{.settings = SettingsTable<>{"o"}, .banks = Banks<otherOut, otherInsideOut>{}};

// The control's code: written the only way there is.
struct Copy {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (std::size_t member = 0; member < io[out].size(); ++member)
			for (std::uint32_t frame = 0; frame < io.frames(); ++frame) io[out][member][frame] = io[in][member][frame];
#ifdef EAR_CASE_io_bank_the_spec_lacks
		io[otherOut][0][0] = 0.0F;
#endif
#ifdef EAR_CASE_io_write_into_input
		io[in][0][0] = 0.0F;
#endif
#ifdef EAR_CASE_io_misspelt_bank
		io[outt][0][0] = 0.0F;
#endif
#ifdef EAR_CASE_io_not_a_bank
		io[0][0][0] = 0.0F;
#endif
	}
};

// The control's class: every member it may have, each of its one shape.
#ifdef EAR_CASE_class_final
class Good final {
#else
class Good {
#endif
public:
#ifdef EAR_CASE_constructor_neither
	explicit Good(int) {}
#else
	explicit Good(NodeOf<spec>& node) : node_(&node) {}
#endif
	static Members<spec> ports(const Checked&) {
#ifdef EAR_CASE_ports_bank_the_spec_lacks
		return {in(members(1)), out(members(1)), insideOut(members(1)), insideIn(members(1)), otherOut(members(1))};
#elif defined(EAR_CASE_ports_bank_left_out)
		return {in(members(1)), out(members(1)), insideOut(members(1))};
#elif defined(EAR_CASE_ports_bank_twice)
	return {in(members(1)), out(members(1)), insideOut(members(1)), in(members(1))};
#else
	return {in(members(1)), out(members(1)), insideOut(members(1)), insideIn(members(1))};
#endif
	}
#ifndef EAR_CASE_private_check
	static std::expected<void, std::string> check(const Checked&) { return {}; }
#endif
#ifdef EAR_CASE_configure_wrong_signature
	Configured configure(CreateOf<spec>& cx, Checked) {
#elif defined(EAR_CASE_configure_not_a_member)
	static Configured configure(CreateOf<spec>& cx, const Checked&) {
#else
Configured configure(CreateOf<spec>& cx, const Checked&) {
#endif
#ifdef EAR_CASE_create_has_no_hold
		cx.hold(out);
#endif
#ifdef EAR_CASE_create_has_no_when_ready
		cx.whenReady([] {});
#endif
#ifdef EAR_CASE_create_has_no_unclaim
		cx.unclaim(insideOut);
#endif
#ifdef EAR_CASE_configured_has_no_restart
		return Configured::restart("x");
#endif
#ifdef EAR_CASE_sound_card_on_public_bank
		cx.soundCard(out, SoundCardShape{.card = *CardId::parse("C")});
#endif
#ifdef EAR_CASE_rtp_receiver_on_output_bank
		cx.rtpReceiver(insideOut, RtpReceiverShape{.address = *RtpAddress::parse("0.0.0.0"),
		                                           .port = 5004,
		                                           .encoding = RtpEncoding::l24,
		                                           .rate = ear::SampleRate::fixed<48000>(),
		                                           .latencyMs = 100});
#endif
#ifdef EAR_CASE_sink_on_input_bank
		static_cast<void>(cx.sink(in, SinkShape{.frames = 64}));
#endif
#ifdef EAR_CASE_source_on_output_bank
		static_cast<void>(cx.source(insideOut, SourceShape{.frames = 64}));
#endif
#ifdef EAR_CASE_source_on_routable_bank
		static_cast<void>(cx.source(in, SourceShape{.frames = 64}));
#endif
#ifdef EAR_CASE_declare_latency_bank_the_spec_lacks
		cx.declareLatency(in, otherOut, Latency{});
#endif
#ifdef EAR_CASE_declare_latency_backwards
		cx.declareLatency(out, in, Latency{});
#endif
#ifdef EAR_CASE_context_bank_the_spec_lacks
		static_cast<void>(cx.sink(otherInsideOut, SinkShape{.frames = 64}));
#endif
#ifdef EAR_CASE_pass_through_backwards
		cx.use(passThrough({bankPair(out, in)}));
#else
		cx.use(Copy{});
#endif
		return Configured::applied();
	}
	// The change version: optional, never called in 3a; of exactly its shape when given.
#ifdef EAR_CASE_change_version_wrong_signature
	Changed configure(ChangeOf<spec>&, Checked, Checked) { return Changed::applied(); }
#elif defined(EAR_CASE_change_version_wrong_context)
	Changed configure(CreateOf<spec>&, const Checked&, const Checked&) { return Changed::applied(); }
#else
Changed configure(ChangeOf<spec>&, const Checked&, const Checked&) { return Changed::applied(); }
#endif
#ifdef EAR_CASE_start_wrong_signature
	int start() { return 0; }
#elif defined(EAR_CASE_overloaded_start)
	void start() {}
	void start(int) {}
#elif !defined(EAR_CASE_private_start)
void start() {}
#endif
#ifdef EAR_CASE_stop_wrong_signature
	void stop(int) {}
#elif !defined(EAR_CASE_private_stop)
	void stop() noexcept {}
#endif
#ifdef EAR_CASE_legacy_start_on_spec_type
	static StartResult start(StartContext&, const Checked&) { return Copy{}; }
#endif

private:
#ifdef EAR_CASE_private_start
	void start() {}
#endif
#ifdef EAR_CASE_private_stop
	void stop() {}
#endif
#ifdef EAR_CASE_private_check
	static std::expected<void, std::string> check(const Checked&) { return {}; }
#endif
	NodeOf<spec>* node_ = nullptr;
};

[[maybe_unused]] std::unique_ptr<const NodeType> makeGood() { return makeNodeType<spec, Good>(); }

#ifdef EAR_CASE_bank_name_invalid
constexpr auto bad = bank::output<"a.b">;
static_assert(bad.name.size() == 3);
#endif

#ifdef EAR_CASE_banks_named_twice
constexpr auto twin = bank::input<"out">;
constexpr Banks<out, twin> bad{};
#endif

// A code whose process is not void process(Io<spec>, const GraphCycle&) noexcept.
#ifdef EAR_CASE_process_may_throw
struct Throwing {
	void process(Io<spec>, const GraphCycle&) {}
};
[[maybe_unused]] NodeCode bad = Throwing{};
#endif

#ifdef EAR_CASE_class_without_ports
struct NoPorts {
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		cx.use(Copy{});
		return Configured::applied();
	}
};
[[maybe_unused]] auto bad = makeNodeType<spec, NoPorts>();
#endif

#ifdef EAR_CASE_check_wrong_result
struct BadCheck {
	static Members<spec> ports(const Checked&) {
		return {in(members(1)), out(members(1)), insideOut(members(1)), insideIn(members(1))};
	}
	static bool check(const Checked&) { return true; }
	Configured configure(CreateOf<spec>&, const Checked&) { return Configured::applied(); }
};
[[maybe_unused]] auto bad = makeNodeType<spec, BadCheck>();
#endif

// A check taking its settings by another type than const CheckedOf<spec>&: found by its name, refused by its signature
// rather than never run.
#ifdef EAR_CASE_check_takes_mutable_settings
struct MutableCheck {
	static Members<spec> ports(const Checked&) {
		return {in(members(1)), out(members(1)), insideOut(members(1)), insideIn(members(1))};
	}
	static std::expected<void, std::string> check(Checked&) { return {}; }
	Configured configure(CreateOf<spec>&, const Checked&) { return Configured::applied(); }
};
[[maybe_unused]] auto bad = makeNodeType<spec, MutableCheck>();
#endif

// A code's prepare and release, likewise held to their signatures.
#ifdef EAR_CASE_prepare_wrong_signature
struct BadPrepare : Copy {
	void prepare(AudioFormat) {}
};
[[maybe_unused]] NodeCode bad = BadPrepare{};
#endif
#ifdef EAR_CASE_release_may_throw
struct ThrowingRelease : Copy {
	void release() {}
};
[[maybe_unused]] NodeCode bad = ThrowingRelease{};
#endif

#ifdef EAR_CASE_harness_bank_the_spec_lacks
[[maybe_unused]] void bad() {
	testing::TypeHarness<spec, Good> harness;
	static_cast<void>(harness.output(otherOut, 0));
}
#endif

#ifdef EAR_CASE_harness_output_as_input
[[maybe_unused]] void bad() {
	testing::TypeHarness<spec, Good> harness;
	static_cast<void>(harness.input(out, 0));
}
#endif

// Loops (docs/writing-a-node-type.md#loops): the control declares a stateful dedicated loop and a strand without a
// state, gives the one its state, posts to each and keeps an executor; the cases name a loop wrongly, give a state where
// none is declared, or post a function of another state.
struct WorkerState {
	void work() {}
};
struct OtherState {};
constexpr auto worker = loop::dedicated<"worker", WorkerState>;
constexpr auto helper = loop::strand<"helper">;
constexpr auto otherLoop = loop::strand<"other">;
#ifdef EAR_CASE_loop_name_invalid
[[maybe_unused]] constexpr auto badLoop = loop::strand<"Bad_Name">;
#endif
#ifdef EAR_CASE_loops_named_twice
[[maybe_unused]] constexpr Loops<helper, loop::dedicated<"helper">> twice{};
#endif
constexpr TypeSpec loopSpec{.settings = SettingsTable<>{"t"}, .banks = Banks<out>{}, .loops = Loops<worker, helper>{}};
struct Zeros {
	void process(Io<loopSpec> io, const GraphCycle&) noexcept {
		for (auto member : io[out])
			for (float& sample : member) sample = 0.0F;
	}
};
class Looped {
public:
	explicit Looped(NodeOf<loopSpec>& node) : node_(node) {}
	static Members<loopSpec> ports(const CheckedOf<loopSpec>&) { return {out(members(1))}; }
	Configured configure(CreateOf<loopSpec>& cx, const CheckedOf<loopSpec>&) {
		cx.loop(worker, std::make_unique<WorkerState>());
		cx.post(worker, [](WorkerState& state) { state.work(); });
		cx.post(helper, [] {});
		static_cast<void>(cx.executor(helper));
#ifdef EAR_CASE_state_to_stateless_loop
		cx.loop(helper, std::make_unique<NoState>());
#endif
#ifdef EAR_CASE_executor_loop_the_spec_lacks
		static_cast<void>(cx.executor(otherLoop));
#endif
		cx.use(Zeros{});
		return Configured::applied();
	}
	void start() {
		node_.post(worker, [](WorkerState& state) { state.work(); });
#ifdef EAR_CASE_post_other_loop_state
		node_.post(worker, [](OtherState&) {});
#endif
#ifdef EAR_CASE_post_loop_the_spec_lacks
		node_.post(otherLoop, [] {});
#endif
	}

private:
	NodeOf<loopSpec>& node_;
};
[[maybe_unused]] void loopedType() { static_cast<void>(makeNodeType<loopSpec, Looped>()); }
#ifdef EAR_CASE_harness_loop_the_spec_lacks
[[maybe_unused]] void badLoopHarness() {
	testing::TypeHarness<loopSpec, Looped> harness;
	static_cast<void>(harness.armed(otherLoop));
}
#endif

} // namespace type_spec_bad

// A spec, or a row its table lists, in an unnamed namespace: GCC 14 would give two files' same-named ones one symbol.
#ifdef EAR_CASE_spec_in_unnamed_namespace
namespace {
using namespace ear;
constexpr TypeSpec hidden{.settings = SettingsTable<>{"t"}, .banks = Banks<type_spec_bad::in>{}};
struct Hidden {
	static Members<hidden> ports(const CheckedOf<hidden>&) { return {type_spec_bad::in(members(1))}; }
	Configured configure(CreateOf<hidden>&, const CheckedOf<hidden>&) { return Configured::applied(); }
};
[[maybe_unused]] auto bad = makeNodeType<hidden, Hidden>();
} // namespace
#endif
#ifdef EAR_CASE_row_in_unnamed_namespace
namespace {
using namespace ear;
constexpr auto hiddenRow = setting::optional("n", setting::number(0, 1));
[[maybe_unused]] constexpr SettingsTable<hiddenRow> bad{"t"};
} // namespace
#endif
