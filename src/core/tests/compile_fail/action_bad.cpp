// What must not compile about a type's actions (ear/core/action_spec.hpp, ear/core/type_spec.hpp;
// docs/writing-a-node-type.md#actions). src/core/CMakeLists.txt registers one CTest test per case, each with the reason
// it must be refused. With no case defined this is the control, and it must compile: what the cases break is only what
// they name.
#include <algorithm>
#include <expected>
#include <memory>
#include <string>

#define EAR_NODE_TYPE_NAME "probe" // as ear_node_type() gives a type's library: the table is named for its folder
#include "ear/core/lifecycle.hpp"

namespace action_bad {

using namespace ear;

struct Worker;
constexpr SettingsTable<> table{"probe"};
constexpr auto in = bank::input<"in">;
constexpr auto out = bank::output<"out">;
constexpr auto worker = loop::strand<"worker", Worker>;
constexpr auto elsewhere = loop::strand<"other", Worker>;
constexpr auto serverRow = setting::required("server", setting::number(1, 100));
constexpr SettingsTable<serverRow> forgetArgs{"forget"};
constexpr auto reset = action::trigger<"reset">("clear the latch");
constexpr auto forget = action::call<"forget", forgetArgs, worker>("forget a server");
#ifdef EAR_CASE_action_arg_marked
// A call's arguments never change, so none carries a change mark (docs/writing-a-node-type.md#change-marks).
constexpr auto markedRow = setting::required("server", setting::number(1, 100)).inPlace();
constexpr SettingsTable<markedRow> markedArgs{"marked"};
constexpr auto marked = action::call<"marked", markedArgs, worker>("forget a server");
#endif
#ifdef EAR_CASE_action_arg_indexed
// A call's arguments are a fixed set of keys: no family (docs/writing-a-node-type.md#indexed-families).
constexpr auto countArg = setting::defaulted("count", setting::number(1, 4), 1);
constexpr auto familyArg = setting::indexed("server", countArg, setting::number(1, 100), 1);
constexpr SettingsTable<countArg, familyArg> familyArgs{"family"};
constexpr auto family = action::call<"family", familyArgs, worker>("forget servers");
#endif
#ifdef EAR_CASE_action_named_lifecycle
// start, stop, restart, disable and enable are the framework's requests.
constexpr auto restartAction = action::trigger<"restart">("restart it");
#endif
#ifdef EAR_CASE_action_name_not_kebab
constexpr auto badName = action::trigger<"resetClip">("clear the latch");
#endif
#ifdef EAR_CASE_call_loop_not_in_spec
// A call to a loop the spec does not list.
constexpr auto lost = action::call<"lost", forgetArgs, elsewhere>("go elsewhere");
constexpr TypeSpec spec{.settings = table,
                        .banks = Banks<in, out>{},
                        .loops = Loops<worker>{},
                        .actions = ActionTable<reset, forget, lost>{}};
#else
constexpr TypeSpec spec{
	.settings = table, .banks = Banks<in, out>{}, .loops = Loops<worker>{}, .actions = ActionTable<reset, forget>{}};
#endif

struct Worker {
#ifdef EAR_CASE_call_handler_signature
	// The handler takes the call's checked arguments by const reference, and answers std::expected<void, std::string>.
	std::expected<void, std::string> act(Invoked<forget>, CheckedOf<forgetArgs> args) {
		static_cast<void>(args);
		return {};
	}
#else
	std::expected<void, std::string> act(Invoked<forget>, const CheckedOf<forgetArgs>& args) {
		static_cast<void>(args.get<serverRow>());
		return {};
	}
#endif
};

struct Code {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (const auto member : io[out]) std::ranges::fill(member, 0.0F);
		static_cast<void>(io.triggered<reset>());
#ifdef EAR_CASE_triggered_not_a_trigger
		// A call reaches its loop's state, never the code.
		static_cast<void>(io.triggered<forget>());
#endif
	}
};

struct Probe {
	static Members<spec> ports(const CheckedOf<spec>&) { return {in(members(1)), out(members(1))}; }
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
#ifdef EAR_CASE_pass_through_with_trigger
		// A pass-through reads no trigger: the spec's trigger could never be seen.
		return cx.applied(passThrough(bankPair(in, out)), cx.state(worker));
#else
		return cx.applied(Code{}, cx.state(worker));
#endif
	}
};

std::unique_ptr<const NodeType> probe() { return makeNodeType<spec, Probe>(); }

} // namespace action_bad
