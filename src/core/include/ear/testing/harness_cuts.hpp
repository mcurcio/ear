// The type harness's record of which cycles the driver cut short (docs/writing-a-node-type.md#lost-cycles): the
// harness's fake block reads it through its CycleAccount, and a test that runs CycleAccount by hand may keep one too.
// It is the engine's own record (ear/core/cut_ring.hpp), told as the engine's driver tells it: a cycle starts, then
// may be marked cut before the next starts.
#pragma once
#include "ear/core/cut_ring.hpp"
namespace ear::testing {

using HarnessCuts = CutRing;

} // namespace ear::testing
