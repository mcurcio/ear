// What must not compile about a type spec, its behaviour, Io and the harness (src/core/CMakeLists.txt registers one
// CTest test per case, each with the reason it must be refused; docs/design/node-framework.md sections 7.1, 7.2 and
// 9.2). With no case defined this is the control, and it must compile: what the cases break is only what they name.
#include <cstdint>
#include <expected>
#include <span>
#include <string>

// As ear_node_type() gives a type's library: a spec's table must be named for its folder.
#ifdef EAR_CASE_spec_not_named_for_folder
#define EAR_NODE_TYPE_NAME "folder"
#else
#define EAR_NODE_TYPE_NAME "t"
#endif
#include "ear/core/type_spec.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace type_spec_bad {

using namespace ear;

constexpr auto in = bank::input<"in">;
constexpr auto out = bank::output<"out">;
constexpr TypeSpec spec{.settings = SettingsTable<>{"t"}, .banks = Banks<in, out>{}};
// Another type's spec, with a bank this spec lacks: of the same direction, under another name. (A bank of the same name,
// direction and reach is the same handle, and compiles: type_spec_test.cpp shows it.)
constexpr auto otherOut = bank::output<"other">;
constexpr TypeSpec otherSpec{.settings = SettingsTable<>{"o"}, .banks = Banks<otherOut>{}};

// The control: a type written the only way there is.
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

struct Good {
	static Members<spec> ports(const CheckedOf<spec>&) {
#ifdef EAR_CASE_ports_bank_the_spec_lacks
		return {in(members(1)), out(members(1)), otherOut(members(1))};
#elif defined(EAR_CASE_ports_bank_left_out)
		return {in(members(1))};
#elif defined(EAR_CASE_ports_bank_twice)
		return {in(members(1)), in(members(1))};
#else
		return {in(members(1)), out(members(1))};
#endif
	}
	static StartResult start(StartContext&, const CheckedOf<spec>&) {
#ifdef EAR_CASE_pass_through_backwards
		return passThrough({bankPair(out, in)});
#else
		return Copy{};
#endif
	}
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
[[maybe_unused]] RunCode bad = Throwing{};
#endif

#ifdef EAR_CASE_behaviour_without_ports
struct NoPorts {
	static StartResult start(StartContext&, const CheckedOf<spec>&) { return Copy{}; }
};
[[maybe_unused]] auto bad = makeNodeType<spec, NoPorts>();
#endif

#ifdef EAR_CASE_check_wrong_result
struct BadCheck : Good {
	static bool check(const CheckedOf<spec>&) { return true; }
};
[[maybe_unused]] auto bad = makeNodeType<spec, BadCheck>();
#endif

// A check taking its settings by another type than const CheckedOf<spec>&: found by its name, refused by its signature
// rather than never run.
#ifdef EAR_CASE_check_takes_mutable_settings
struct MutableCheck : Good {
	static std::expected<void, std::string> check(CheckedOf<spec>&) { return {}; }
};
[[maybe_unused]] auto bad = makeNodeType<spec, MutableCheck>();
#endif

// A code's prepare and release, likewise held to their signatures.
#ifdef EAR_CASE_prepare_wrong_signature
struct BadPrepare : Copy {
	void prepare(AudioFormat) {}
};
[[maybe_unused]] RunCode bad = BadPrepare{};
#endif
#ifdef EAR_CASE_release_may_throw
struct ThrowingRelease : Copy {
	void release() {}
};
[[maybe_unused]] RunCode bad = ThrowingRelease{};
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

} // namespace type_spec_bad

// A spec, or a row its table lists, in an unnamed namespace: GCC 14 would give two files' same-named ones one symbol.
#ifdef EAR_CASE_spec_in_unnamed_namespace
namespace {
using namespace ear;
constexpr TypeSpec hidden{.settings = SettingsTable<>{"t"}, .banks = Banks<type_spec_bad::in>{}};
struct Hidden {
	static Members<hidden> ports(const CheckedOf<hidden>&) { return {type_spec_bad::in(members(1))}; }
	static StartResult start(StartContext&, const CheckedOf<hidden>&) { return passThrough({}); }
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
