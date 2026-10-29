#ifndef EAR_GRAPH_CONFIG_HPP
#define EAR_GRAPH_CONFIG_HPP

#include "graph.hpp"
#include "pipe_source.hpp"

#include "nlohmann/json.hpp"

#include <iostream>
#include <memory>
#include <string>

namespace ear {

// Builds the audio graph that the config describes onto `device`: the device's output node,
// one source node per entry in "nodes", and each entry's "connect" pairs
// ([node output channel, device output channel]). The graph is prepared when the device
// starts (AudioGraphStandalone::audioDeviceAboutToStart).
// Returns an empty string on success, otherwise what went wrong.
inline std::string buildGraphFromConfig(AudioGraphStandalone& graph, const std::shared_ptr<AudioIoDevice>& device, const nlohmann::json& config) {
	auto outputNode = graph.setDevice(device);

	std::cout << "Registered output device with graph" << std::endl;

	if (!config.count("nodes")) {
		return "No nodes found in config";
	}

	unsigned airplayBufferSize = 40;
	if (config.count("airplay")) {
		const nlohmann::json& airplayConfig = config["airplay"];
		if (airplayConfig.count("buffer")) {
			airplayBufferSize = airplayConfig["buffer"].get<unsigned>();
		}
	}

	for (auto& nodeConfig : config["nodes"]) {
		std::unique_ptr<GraphSource> node;

		const std::string type = nodeConfig["type"];
		if (type == "white") {
			DBG("Creating white noise node");
			node = std::make_unique<WhiteNoiseProcessor>();
		} else if (type == "airplay") {
			DBG(juce::String("Creating AirPlay node from pipe ") + juce::String(nodeConfig["pipe"].get<std::string>()));
			node = std::make_unique<AirplayProcessor>(nodeConfig["pipe"].get<std::string>(), airplayBufferSize);
		} else {
			return "Unknown node type: " + type;
		}

		auto handle = graph.addNode(std::move(node));

		for (auto& it : nodeConfig["connect"].items()) {
			const nlohmann::json connection = it.value();
			const int nodeOutputChannel = connection[0].get<int>();
			const int outputInputChannel = connection[1].get<int>();

			std::cout << "connecting node output " << nodeOutputChannel << " to device output " << outputInputChannel << std::endl;
			if (!graph.addConnection({{handle, nodeOutputChannel}, {outputNode, outputInputChannel}})) {
				return "Failed to connect node output " + std::to_string(nodeOutputChannel)
					+ " to device output " + std::to_string(outputInputChannel);
			}
		}
	}

	return {};
}

} // namespace ear

#endif
