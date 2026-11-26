// Test support: a Report<spec> over a handle a test made, for a loop's state a test runs outside the type harness on
// loops of its own (the sendspin clock tests' worker). The test's handle overrides NodeHandle's reportStatus and
// reportEvent to hear what the state writes, by field and event index (StatusOf<spec>::declared, EventsOf<spec>::
// declared name them); one that does not drops them, as a handle no NodeHost made does
// (docs/writing-a-node-type.md#report).
#pragma once
#include "ear/core/node_handle.hpp"
#include "ear/core/report.hpp"
namespace ear::testing {
struct ReportSeam {
	template <const auto& Spec> static Report<Spec> on(NodeHandle& handle) noexcept { return Report<Spec>(handle); }
};
/// A Report of `Spec` whose writes reach `handle`.
template <const auto& Spec> Report<Spec> reportOn(NodeHandle& handle) noexcept { return ReportSeam::on<Spec>(handle); }
} // namespace ear::testing
