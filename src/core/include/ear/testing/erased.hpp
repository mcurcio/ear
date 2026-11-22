// The erased layer's applied answer for the framework's own tests (docs/testing.md): a test's hand-made
// ErasedInstance, and the helpers and lambdas that answer for one, make it here. A type's class cannot: it answers
// `cx.applied(...)` (docs/writing-a-node-type.md#configure-and-createof), and a guard refuses this header and the
// erased layer's names under src/nodes/.
#pragma once
#include "ear/core/lifecycle.hpp"
namespace ear::testing {
/// ErasedInstance::applied, public: `Erased::applied(code, states...)`. Never made.
struct Erased : ErasedInstance {
	using ErasedInstance::applied;
};
} // namespace ear::testing
