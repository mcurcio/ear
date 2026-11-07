// The "white" node type: one output, "noise.mono", carrying uniform white noise in [-1, 1). It takes
// no settings (docs/design/m1-white-to-file.md section 2, ear/nodes/white.hpp; written against its spec,
// docs/design/node-framework.md section 7.1).
#include <cstdint>
#include <memory>
#include <random>
#include <span>

#include "ear/core/type_spec.hpp"
#include "ear/nodes/white.hpp"

namespace ear::nodes {
namespace {

constexpr auto noise = bank::output<"noise">;
constexpr TypeSpec spec{.settings = SettingsTable<>{"white"}, .banks = Banks<noise>{}};

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

} // namespace

std::unique_ptr<const NodeType> makeWhiteNodeType() { return makeNodeType<spec, White>(); }

} // namespace ear::nodes
