// The "white" node type's spec and behaviour (docs/design/m1-white-to-file.md section 2; docs/design/node-framework.md
// sections 7.1 and 7.6): private to the type, shared by white.cpp and its tests, which run it through
// ear::testing::TypeHarness<spec, White> by the same bank handles. In the type's own named namespace, never an unnamed
// one (type_spec.hpp says why).
#pragma once
#include <cstdint>
#include <random>
#include <span>

#include "ear/core/type_spec.hpp"

namespace ear::nodes::white {

inline constexpr auto noise = bank::output<"noise">;
inline constexpr TypeSpec spec{.settings = SettingsTable<>{"white"}, .banks = Banks<noise>{}};

struct WhiteCode {
	std::mt19937 generator{std::random_device{}()};

	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (const std::span<float> member : io[noise]) {
			for (float& sample : member) sample = next();
		}
	}

	// The top 24 bits of the generator's 32 are exact in a float: scaled by 2^-23 they give
	// [0, 2), so the sample lies in [-1, 1) and never reaches 1 (std::uniform_real_distribution
	// may round up to its upper bound with float).
	float next() noexcept {
		constexpr float scale = 1.0F / static_cast<float>(1U << 23U);
		return static_cast<float>(generator() >> 8U) * scale - 1.0F;
	}
};

struct White {
	static Members<spec> ports(const CheckedOf<spec>&) { return {noise(members("mono"))}; }
	static StartResult start(StartContext&, const CheckedOf<spec>&) { return WhiteCode{}; }
};

} // namespace ear::nodes::white
