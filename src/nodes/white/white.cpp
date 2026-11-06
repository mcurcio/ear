// The "white" node type: one output, "noise.mono", carrying uniform white noise in [-1, 1). It takes
// no settings (docs/design/m1-white-to-file.md section 2, ear/nodes/white.hpp).
#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ear/core/settings_table.hpp"
#include "ear/nodes/white.hpp"

namespace ear::nodes {
namespace {

constexpr std::string_view typeName = "white";
constexpr SettingsTable table{typeName, std::array<SettingRule, 0>{}};

class WhiteNoise final : public AudioCode {
public:
	WhiteNoise() : generator_(std::random_device{}()) {}

	void prepare(const AudioFormat&) override {}

	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle&) noexcept override {
		for (const std::span<float> output : outputs) {
			for (float& sample : output.first(frames)) {
				sample = next();
			}
		}
	}

	void release() noexcept override {}

private:
	// The top 24 bits of the generator's 32 are exact in a float: scaled by 2^-23 they give
	// [0, 2), so the sample lies in [-1, 1) and never reaches 1 (std::uniform_real_distribution
	// may round up to its upper bound with float).
	float next() noexcept {
		constexpr float scale = 1.0F / static_cast<float>(1U << 23U);
		return static_cast<float>(generator_() >> 8U) * scale - 1.0F;
	}

	std::mt19937 generator_;
};

class WhiteInsides final : public NodeInsides {
public:
	AudioCode& audioCode() override { return noise_; }

private:
	WhiteNoise noise_;
};

class WhiteConfig final : public NodeConfig {
public:
	const Ports& ports() const override { return ports_; }

	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant&) const override {
		return std::make_unique<WhiteInsides>();
	}

private:
	Ports ports_{{{Direction::output, "noise", {"mono"}}}};
};

class WhiteType final : public NodeType {
public:
	std::string_view name() const override { return typeName; }

	std::span<const SettingDeclaration> settingKinds() const override { return table.kinds(); }

	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
	parse(const NodeName& node, const NodeSettings& settings) const override {
		auto checked = table.check(settings);
		if (!checked) return std::unexpected(InvalidSettings{node, std::move(checked.error())});
		return std::make_unique<const WhiteConfig>();
	}
};

} // namespace

std::unique_ptr<const NodeType> makeWhiteNodeType() { return std::make_unique<const WhiteType>(); }

} // namespace ear::nodes
