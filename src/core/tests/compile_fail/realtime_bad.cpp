// What must not compile about what a code writes on the realtime thread through Io, and what Report takes
// (ear/core/type_spec.hpp, ear/core/report.hpp; docs/writing-a-node-type.md#reporting-from-the-code). src/core/CMakeLists.txt
// registers one CTest test per case, each with the reason it must be refused. With no case defined this is the
// control, and it must compile: what the cases break is only what they name.
#include <algorithm>
#include <memory>
#include <string_view>

#define EAR_NODE_TYPE_NAME "probe" // as ear_node_type() gives a type's library: the table is named for its folder
#include "ear/core/lifecycle.hpp"
#include "ear/core/report.hpp"

namespace realtime_bad {

using namespace ear;

constexpr SettingsTable<> table{"probe"};
constexpr auto in = bank::input<"in">;
constexpr auto out = bank::output<"out">;
constexpr auto worker = loop::strand<"worker">;
constexpr std::string_view modes[] = {"quiet", "loud"};
constexpr auto level = status::number<"level">(writer::rt, 0, 10);
constexpr auto connected = status::flag<"connected">(writer::loop(worker));
constexpr auto modeArg = arg::choice<"mode">(modes);
constexpr auto clipped =
	event::of<"clipped">(EventSeverity::warn, "clipped {n} in {mode}", arg::number<"n">(0, 100), modeArg);
constexpr auto said = event::of<"said">(EventSeverity::info, "said {what}", arg::text<"what">());
constexpr auto overrun = log::code<"overrun">(LogLevel::debug, "overrun by {n}", arg::number<"n">(0, 100));
constexpr TypeSpec spec{.settings = table,
                        .banks = Banks<in, out>{},
                        .loops = Loops<worker>{},
                        .status = StatusTable<level, connected>{},
                        .events = EventTable<clipped, said>{},
                        .logCodes = LogCodes<overrun>{}};

// Another spec's own field, event and log code.
constexpr SettingsTable<> otherTable{"other"};
constexpr auto otherLevel = status::number<"level">(writer::rt, 0, 10);
constexpr auto otherClipped = event::of<"clipped">(EventSeverity::warn, "clipped");
constexpr auto otherOverrun = log::code<"overrun">(LogLevel::debug, "overrun");
constexpr TypeSpec otherSpec{.settings = otherTable,
                             .banks = Banks<out>{},
                             .status = StatusTable<otherLevel>{},
                             .events = EventTable<otherClipped>{},
                             .logCodes = LogCodes<otherOverrun>{}};

struct Code {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (const auto member : io[out]) std::ranges::fill(member, 0.0F);
		io.set<level>(3);
		io.raise<clipped>(1, modeArg("loud"));
		io.log<overrun>(2);
#ifdef EAR_CASE_set_field_not_in_spec
		// A field of another spec, though of the same name.
		io.set<otherLevel>(3);
#endif
#ifdef EAR_CASE_set_field_not_rt
		// A loop's field is written through Report, from its loop.
		io.set<connected>(true);
#endif
#ifdef EAR_CASE_raise_text_arg_from_process
		// The realtime thread writes no text.
		io.raise<said>(std::string_view("hello"));
#endif
#ifdef EAR_CASE_raise_wrong_arg_count
		io.raise<clipped>(1);
#endif
#ifdef EAR_CASE_raise_event_not_in_spec
		io.raise<otherClipped>();
#endif
#ifdef EAR_CASE_raise_choice_by_text
		// From process a choice is its consteval-made value, never its text.
		io.raise<clipped>(1, std::string_view("loud"));
#endif
#ifdef EAR_CASE_log_code_not_in_spec
		io.log<otherOverrun>();
#endif
#ifdef EAR_CASE_choice_arg_misspelt
		io.raise<clipped>(1, modeArg("lound"));
#endif
	}
};

struct Probe {
	explicit Probe(NodeOf<spec>& node) : report(node.report()) {
#ifdef EAR_CASE_report_set_rt_field
		// An rt field is the code's, through Io.
		report.set<level>(3);
#endif
#ifdef EAR_CASE_report_of_other_spec
		report.set<otherLevel>(3);
#endif
	}
	static Members<spec> ports(const CheckedOf<spec>&) { return {in(members(1)), out(members(1))}; }
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
#ifdef EAR_CASE_pass_through_with_rt
		// A pass-through writes no rt field and no log code: the spec could never be served.
		return cx.applied(passThrough(bankPair(in, out)));
#else
		return cx.applied(Code{});
#endif
	}
	Report<spec> report;
};

#ifdef EAR_CASE_report_constructed
// A Report is the configuration's, given by NodeOf::report; it is never made from a handle.
Report<spec> made(NodeHandle& handle) { return Report<spec>(handle); }
#endif

std::unique_ptr<const NodeType> probe() { return makeNodeType<spec, Probe>(); }

} // namespace realtime_bad
