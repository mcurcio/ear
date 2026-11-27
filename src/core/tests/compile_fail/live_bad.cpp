// What must not compile about live rows (docs/writing-a-node-type.md#live-rows): their marks, their reads and io.param
// (src/core/CMakeLists.txt registers one CTest test per case, each with the reason it must be refused). With no case
// defined this is the control, and it must compile: what the cases break is only what they name.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ear/core/lifecycle.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace live_bad {

using namespace ear;

std::optional<std::string> anyText(std::string_view text) { return std::string(text); }
enum class Shape { peak, shelf };

inline constexpr auto countRow = setting::defaulted("n", setting::number(1, 4), 1u).inPlace();
inline constexpr auto stepCountRow = setting::defaulted("k", setting::number(1, 4), 1u).live(setting::step);
inline constexpr auto fixedRow = setting::defaulted("fixed", setting::decimal(0.0, 1.0), 0.0).inPlace();
inline constexpr auto levelRow =
	setting::defaulted("level", setting::decimal(-90.0, 24.0, "dB"), 0.0).live(setting::ramp(20));
inline constexpr auto trimRow =
	setting::indexed("trim", countRow, setting::decimal(-24.0, 24.0, "dB"), 0.0).live(setting::ramp(20));
inline constexpr auto bandRow = setting::indexed("band", stepCountRow, setting::boolean(), false).live(setting::step);
inline constexpr auto requiredRow = setting::required("r", setting::number(0, 9)).live(setting::step);
inline constexpr auto shapeRow =
	setting::defaulted("shape", setting::choice<Shape>({{"peak", Shape::peak}, {"shelf", Shape::shelf}}), Shape::peak)
		.live(setting::step);
inline constexpr auto otherRow = setting::defaulted("other", setting::decimal(0.0, 1.0), 0.0).live(setting::ramp(5));

inline constexpr auto out = bank::output<"out">;
inline constexpr TypeSpec spec{
	.settings = SettingsTable<countRow, stepCountRow, fixedRow, levelRow, trimRow, bandRow, requiredRow, shapeRow>{"t"},
	.banks = Banks<out>{}};
using Checked = CheckedOf<spec>;

// The control reads each live row the one way there is.
inline std::uint32_t reads(const Checked& s) {
	return static_cast<std::uint32_t>(s.liveValue<levelRow>() + s.liveValue<trimRow>(0)) + s.count<trimRow>() +
	       s.liveCount<bandRow>() + static_cast<std::uint32_t>(s.get<fixedRow>());
}

struct Code {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		const Smoothed level = io.param<levelRow>();
		const Smoothed trim = io.param<trimRow>(0);
		const Stepped<bool> band = io.param<bandRow>(1);
		const Stepped<Shape> shape = io.param<shapeRow>();
		const std::uint32_t bands = io.count<bandRow>() + io.count<trimRow>();
		for (std::uint32_t i = 0; i < io.frames(); ++i)
			io[out][0][i] = level[i] + trim[i] + (band.value() ? 1.0F : 0.0F) +
			                (shape.value() == Shape::shelf ? 1.0F : 0.0F) + static_cast<float>(bands);
#ifdef EAR_CASE_param_not_live
		static_cast<void>(io.param<fixedRow>());
#endif
#ifdef EAR_CASE_param_row_not_in_spec
		static_cast<void>(io.param<otherRow>());
#endif
#ifdef EAR_CASE_param_family_without_member
		static_cast<void>(io.param<trimRow>());
#endif
#ifdef EAR_CASE_param_member_of_plain_row
		static_cast<void>(io.param<levelRow>(0));
#endif
	}
};
struct Type {
	static Members<spec> ports(const Checked&) { return {out(members(1))}; }
	Configured configure(CreateOf<spec>& cx, const Checked& s) {
		static_cast<void>(reads(s));
#ifdef EAR_CASE_get_live_row
		static_cast<void>(s.get<levelRow>());
#endif
#ifdef EAR_CASE_count_live_row
		static_cast<void>(s.count<bandRow>());
#endif
#ifdef EAR_CASE_live_value_not_live
		static_cast<void>(s.liveValue<fixedRow>());
#endif
		return cx.applied(Code{});
	}
};
inline const auto made = makeNodeType<spec, Type>();

// The marks.
#ifdef EAR_CASE_live_text_row
inline constexpr auto bad = setting::required("t", setting::text<anyText>("anything")).live(setting::step);
#endif
#ifdef EAR_CASE_live_rate_row
inline constexpr auto bad =
	setting::required("r", setting::rate(SampleRate::fixed<8000>(), SampleRate::fixed<192000>())).live(setting::step);
#endif
#ifdef EAR_CASE_live_optional_row
inline constexpr auto bad = setting::optional("o", setting::number(1, 2)).live(setting::step);
#endif
#ifdef EAR_CASE_ramp_whole_number
inline constexpr auto bad = setting::defaulted("w", setting::number(1, 2), 1u).live(setting::ramp(10));
#endif
#ifdef EAR_CASE_ramp_choice
inline constexpr auto bad =
	setting::defaulted("c", setting::choice<Shape>({{"peak", Shape::peak}}), Shape::peak).live(setting::ramp(10));
#endif
#ifdef EAR_CASE_ramp_zero
inline constexpr auto bad = setting::defaulted("z", setting::decimal(0.0, 1.0), 0.0).live(setting::ramp(0));
#endif
#ifdef EAR_CASE_live_shapes_ports
inline constexpr auto bad = setting::defaulted("s", setting::number(1, 2), 1u).live(setting::step).shapesPorts();
#endif
#ifdef EAR_CASE_live_and_in_place
inline constexpr auto bad = setting::defaulted("s", setting::number(1, 2), 1u).inPlace().live(setting::step);
#endif
#ifdef EAR_CASE_live_members_over_256
inline constexpr auto manyCount = setting::defaulted("m", setting::number(1, 200), 1u).inPlace();
inline constexpr auto many = setting::indexed("a", manyCount, setting::decimal(0.0, 1.0), 0.0).live(setting::step);
inline constexpr auto more = setting::indexed("b", manyCount, setting::decimal(0.0, 1.0), 0.0).live(setting::step);
inline constexpr SettingsTable<manyCount, many, more> bad{"t"};
#endif

} // namespace live_bad
