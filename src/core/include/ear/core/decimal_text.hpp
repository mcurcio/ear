#pragma once
// A decimal as ear writes it (docs/graph-file.md#settings-and-their-kinds): in ear.conf and in a decimal row's
// refusal. The one home of that form.
#include <string>
namespace ear {

/// `value` as the shortest text in fixed notation that reads back as the same double: -3.5, -6 for -6.0, 0.25, 1000
/// for 1e3, 0.0001 for 1e-4, and 0 for a negative zero. `value` is finite: ear never holds another decimal.
std::string decimalText(double value);
/// `value` rounded to `decimals` places, half away from zero on its decimalText (so -6.05 at one place is -6.1, never
/// what its binary value would round to), and 0 for a negative zero: the value a gesture's end commits
/// (docs/architecture.md#gestures). `value` is finite.
double roundedDecimal(double value, unsigned decimals);

} // namespace ear
