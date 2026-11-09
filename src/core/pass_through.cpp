// The framework's pass-through between banks (pass_through.hpp): the indices are worked out once, from the banks, when
// the run starts; process() only copies.
#include "ear/core/pass_through.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace ear {
namespace {

class PassThroughCode final : public AudioCode {
public:
	struct Copy {
		std::size_t input;
		std::size_t output;
	};
	PassThroughCode(std::vector<Copy> copies, std::vector<std::size_t> silent)
		: copies_(std::move(copies)), silent_(std::move(silent)) {}

	void prepare(const AudioFormat&) override {}

	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle&) noexcept override {
		for (const Copy& copy : copies_)
			std::ranges::copy(inputs[copy.input].first(frames), outputs[copy.output].begin());
		for (const std::size_t output : silent_) std::ranges::fill(outputs[output].first(frames), 0.0F);
	}

	void release() noexcept override {}

private:
	std::vector<Copy> copies_;
	std::vector<std::size_t> silent_;
};

} // namespace

NodeCode NodeCode::passThrough(std::vector<BankPair> pairs) {
	NodeCode code;
	code.pairs_ = std::move(pairs);
	return code;
}

std::unique_ptr<AudioCode> makePassThrough(const Ports& ports, const std::vector<BankPair>& pairs) {
	if (pairs.empty()) throw std::logic_error("a pass-through names at least one pair of banks");
	const std::size_t outputCount = ports.outputs().size() + ports.insideOutputs().size();
	std::vector<bool> written(outputCount, false);
	std::vector<PassThroughCode::Copy> copies;
	for (const BankPair& pair : pairs) {
		const Ports::Bank& from = ports.bank(pair.from);
		const Ports::Bank& to = ports.bank(pair.to);
		if (from.direction != Direction::input || to.direction != Direction::output)
			throw std::logic_error("a pass-through copies an input bank to an output bank: \"" + pair.from +
			                       "\" to \"" + pair.to + "\"");
		if (from.ports.size() != to.ports.size())
			throw std::logic_error("a pass-through's banks are one size: \"" + pair.from + "\" has " +
			                       std::to_string(from.ports.size()) + ", \"" + pair.to + "\" has " +
			                       std::to_string(to.ports.size()));
		for (std::size_t i = 0; i < from.ports.size(); ++i) {
			if (written[to.first + i])
				throw std::logic_error("a pass-through writes bank \"" + pair.to + "\" more than once");
			written[to.first + i] = true;
			copies.push_back({from.first + i, to.first + i});
		}
	}
	std::vector<std::size_t> silent;
	for (std::size_t output = 0; output < outputCount; ++output)
		if (!written[output]) silent.push_back(output);
	return std::make_unique<PassThroughCode>(std::move(copies), std::move(silent));
}

} // namespace ear
