#pragma once
// A rate as ear.conf and the --rate flag write it (docs/graph-file.md#rates): a number, then hz or khz in any case,
// giving a whole number of hertz (48khz, 44.1khz, 48000hz). The one home of that grammar and of the form ear writes.
#include <expected>
#include <string>
#include <string_view>
#include <optional>
#include "ear/core/graph_spec.hpp"
#include "ear/core/sample_rate.hpp"
namespace ear {

/// The rates a reader takes, both ends included.
struct RateRange {
	SampleRate low;
	SampleRate high;
};
/// The graph's rate (ear.conf's graph { rate ...; } and --rate): 8 kHz to 768 kHz.
inline constexpr RateRange graphRates{SampleRate::fixed<8'000>(), SampleRate::fixed<768'000>()};
/// Every rate a SampleRate holds: 1 Hz to 4294967295 Hz.
inline constexpr RateRange anyRate{SampleRate::fixed<1>(), SampleRate::fixed<4'294'967'295>()};

/// `text` as a rate within `range`, or why not, naming `text` as written:
///   "<text> has no unit: write <n>hz or <rateText>" for a bare whole number (one form when both are the same);
///   "<text> has no unit: write hz or khz after it, such as 48khz" for any other bare number;
///   "<text> is not a rate: write a number, then hz or khz, such as 48khz, 44.1khz or 48000hz" for anything else
///   that is not a number and a unit (MHz, a sign, an exponent, a space, a word);
///   "<text> is not a whole number of hertz" (44.1003khz);
///   "<text> is outside <low> to <high>", in rateText's form, for 0 and everything outside the range.
std::expected<SampleRate, std::string> readRate(std::string_view text, RateRange range = anyRate);
/// The form ear writes: "<n>khz" for whole kHz, "<n>.<d>khz" for whole hundreds of Hz (44.1khz), else "<n>hz".
/// readRate gives the same rate back.
std::string rateText(SampleRate rate);
/// "<low> to <high>", each in rateText's form.
std::string rangeText(RateRange range);

/// A rate setting's value in a node's settings, as the control port carries it: a JSON whole number of hertz
/// (docs/graph-file.md#settings-and-their-kinds). The rate's number leaves the type only here and through rateText.
NodeSettings rateSetting(SampleRate rate);
/// The rate a JSON value holds: a whole number from 1 to 4294967295; nothing for anything else (a float, a string).
std::optional<SampleRate> rateOfSetting(const NodeSettings& value);

} // namespace ear
