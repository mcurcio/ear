// What must not compile about a type's status fields, events and log codes (ear/core/status_spec.hpp; src/core/CMakeLists.txt
// registers one CTest test per case, each with the reason it must be refused). With no case defined this is the
// control, and it must compile: what the cases break is only what they name.
#include <string_view>

#include "ear/core/lifecycle.hpp"

namespace status_spec_bad {

using namespace ear;

constexpr SettingsTable<> table{"probe"};
constexpr auto out = bank::output<"out">;
constexpr auto worker = loop::strand<"worker">;
constexpr auto other = loop::strand<"other">;
constexpr std::string_view connections[] = {"dialling", "connected"};
constexpr auto connection = status::choice<"connection">(writer::loop(worker), connections);
constexpr auto level = status::number<"level">(writer::rt, 0, 10);
constexpr auto failed = event::of<"failed">(EventSeverity::warn, "failed: {reason}", arg::text<"reason">());

void control() {
	static constexpr TypeSpec spec{.settings = table,
	                               .banks = Banks<out>{},
	                               .loops = Loops<worker>{},
	                               .status = StatusTable<connection, level>{},
	                               .events = EventTable<failed>{}};
	static_cast<void>(spec);
	static_assert(connection("connected") == Choice{1});
}

#ifdef EAR_CASE_rt_text_field
// A text is never written from the realtime thread: an rt field is one atomic word.
constexpr auto bad = status::text<"name">(writer::rt);
#endif

#ifdef EAR_CASE_writer_loop_not_in_spec
// A field's loop must be one of its spec's loops.
constexpr auto elsewhere = status::flag<"elsewhere">(writer::loop(other));
constexpr TypeSpec bad{
	.settings = table, .banks = Banks<out>{}, .loops = Loops<worker>{}, .status = StatusTable<elsewhere>{}};
#endif

#ifdef EAR_CASE_choice_value_misspelt
// A choice's value is named once, in its field: a misspelt one does not compile.
constexpr Choice bad = connection("conected");
#endif

#ifdef EAR_CASE_initially_not_a_value
// A field's initial value is within its range.
constexpr auto bad = status::number<"level">(writer::rt, 0, 10).initially(11);
#endif

#ifdef EAR_CASE_sentence_unknown_place
// Each {place} of a sentence names an argument.
constexpr auto bad = event::of<"failed">(EventSeverity::warn, "failed: {why}", arg::text<"reason">());
#endif

#ifdef EAR_CASE_sentence_missing_arg
// Each argument has a place in the sentence.
constexpr auto bad = event::of<"failed">(EventSeverity::warn, "failed", arg::text<"reason">());
#endif

#ifdef EAR_CASE_duplicate_status_name
constexpr auto again = status::flag<"connection">(writer::rt);
constexpr StatusTable<connection, again> bad{};
#endif

#ifdef EAR_CASE_duplicate_event_code
constexpr auto again = event::of<"failed">(EventSeverity::info, "failed again");
constexpr EventTable<failed, again> bad{};
#endif

#ifdef EAR_CASE_log_code_text_arg
// The realtime thread writes no text.
constexpr auto bad = log::code<"said">(LogLevel::info, "said {what}", arg::text<"what">());
#endif

#ifdef EAR_CASE_log_code_five_args
constexpr auto bad = log::code<"five">(LogLevel::info, "{a} {b} {c} {d} {e}", arg::flag<"a">(), arg::flag<"b">(),
                                       arg::flag<"c">(), arg::flag<"d">(), arg::flag<"e">());
#endif

} // namespace status_spec_bad

#ifdef EAR_CASE_status_in_unnamed_namespace
namespace {
constexpr auto hidden = ear::status::flag<"hidden">(ear::writer::rt);
constexpr ear::StatusTable<hidden> bad{};
} // namespace
#endif

#ifdef EAR_CASE_event_in_unnamed_namespace
namespace {
constexpr auto hidden = ear::event::of<"hidden">(ear::EventSeverity::info, "hidden");
constexpr ear::EventTable<hidden> bad{};
} // namespace
#endif
