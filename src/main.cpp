#include "server.hpp"

#include <iostream>
#include <thread>

#include <boost/program_options.hpp>

#include "juce_audio_processors_headless/juce_audio_processors_headless.h"

#include "ear/device.hpp"
#include "ear/processor.hpp"
#include "ear/graph.hpp"
#include "ear/sources.hpp"
#include "ear/players.hpp"

#include "nlohmann/json.hpp"
#if 0
int pipeTest(boost::program_options::variables_map&) {
	//boost::asio::io_context io_context;
	ear::AirplaySource source("/tmp/airplay");
	source.prepareToPlay(512, 44100.0);
	//io_context.run();
	std::this_thread::sleep_for(std::chrono::seconds(60));
	source.releaseResources();
}

int simpleTest(boost::program_options::variables_map&) {
	auto devices = ear::AudioIoDevice::getAllDevices();
	auto device = devices[0];
	bool isOpen = device->open();
	if (!isOpen) {
		std::cerr << "Failed to open device:" << device->getName() << std::endl;
		return 1;
	}

	device->start();

	auto juceSource = std::make_shared<ear::FileAudioSource>(juce::File("/Users/matt/Development/ear/media/01 Misery.m4a"));

	ear::AudioIoDeviceFunctorCallback callback([juceSource] (const float* const*, int, float* const*, int outputChannels, int samples) {
		DBG("callback channels=" + juce::String(outputChannels) + " samples=" + juce::String(samples));

		//juce::AudioSampleBuffer buffer(outputs, outputChannels, samples);
		//juce::AudioSourceChannelInfo info(buffer);
		//juceSource->getNextAudioBlock(info);
	});

	//juceSource->prepareToPlay(44100, 512);
	if (!device->addCallback(&callback)) {
		std::cerr << "Failed to register callback" << std::endl;
		return 1;
	}

	juce::AudioSourcePlayer player;
	if (!device->addCallback(&player)) {
		std::cerr << "Failed to register player" << std::endl;
		return 1;
	}

	player.setSource(juceSource.get());

	//player.play(juce::File("/Users/matt/Development/ear/media/01 Misery.m4a"));

	std::this_thread::sleep_for(std::chrono::seconds(5));

	return 0;
}

int customGraphTest(boost::program_options::variables_map& vm) {
	auto devices = ear::AudioIoDevice::getAllDevices();
	auto device = devices[0];
	bool isOpen = device->open();
	if (!isOpen) {
		std::cerr << "Failed to open device:" << device->getName() << std::endl;
		return 1;
	}

	device->start();

	ear::AudioGraph graph;
	//juce::AudioProcessorGraph graph;
	graph.setPlayConfigDetails(0, 1, 44100, 512);

	std::unique_ptr<juce::AudioProcessor> graphInput = std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode);
	auto graphInputNode = graph.addNode(std::move(graphInput));
	//std::cout << "Added GraphInput ID=" << graphInputNode->nodeID.uid << std::endl;

	std::unique_ptr<juce::AudioProcessor> graphOutput = std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode);
	auto graphOutputNode = graph.addNode(std::move(graphOutput));
	//std::cout << "Added GraphOutput ID=" << graphOutputNode->nodeID.uid << std::endl;

	auto juceSource = std::make_shared<ear::FileAudioSource>(juce::File("/Users/matt/Development/ear/media/01 Misery.m4a"));
	std::unique_ptr<juce::AudioProcessor> graphSource = std::make_unique<ear::GraphSource>(juceSource);
	graphSource->setPlayConfigDetails(0, 2, 44100, 512);
	auto graphSourceNode = graph.addNode(std::move(graphSource));
	//std::cout << "Added GraphSource ID=" << graphSourceNode->nodeID.uid << std::endl;

	auto whiteNoiseSource = std::make_shared<ear::WhiteNoiseSource>();
	auto whiteNoiseProcessor = std::make_unique<ear::GraphSource>(whiteNoiseSource);
	whiteNoiseProcessor->setPlayConfigDetails(0, 1, 44100, 512);
	auto whiteNoiseNode = graph.addNode(std::move(whiteNoiseProcessor));

	std::unique_ptr<ear::GainProcessor> gainProcessor = std::make_unique<ear::GainProcessor>();
	gainProcessor->setPlayConfigDetails(1, 1, 44100, 512);
	float gainVal = vm["gain"].as<float>();
	std::cout << "configured gain: " << gainVal << std::endl;
	gainProcessor->setGainDecibels(gainVal);
	double rampVal = vm["ramp"].as<double>();
	std::cout << "configured ramp: " << rampVal << std::endl;
	gainProcessor->setRampDurationSeconds(rampVal);
	auto gainNode = graph.addNode(std::move(gainProcessor));
	//std::cout << "Added GainNode ID=" << gainNode->nodeID.uid << std::endl;

	assert(graph.addConnection({{whiteNoiseNode, 0}, {gainNode, 0}}));

	ear::AudioGraph::Connection connection1({graphSourceNode, vm["channel"].as<int>()}, {gainNode, 0});
	if (!graph.canConnect(connection1)) {
		std::cerr << " cannot connect nodes" << std::endl;
		if (vm.count("quit")) {
			return 1;
		}
		//return 1;
	}
	if (!graph.addConnection(connection1)) {
		std::cerr << "did not connect nodes" << std::endl;
		if (vm.count("quit")) {
			return 1;
		}
	}

	if (vm.count("connect")) {

		ear::AudioGraph::Connection connection({gainNode, 0}, {graphOutputNode, 0});
		std::cout << "device outputchannels=" << gainNode.getOutputChannelCount()
			<< " inputchannels=" << graphOutputNode.getInputChannelCount() << std::endl;
		return 0;
		if (!graph.canConnect(connection)) {
			std::cerr << " cannot connect nodes" << std::endl;
			if (vm.count("quit")) {
				return 1;
			}
			//return 1;
		}
		if (!graph.addConnection(connection)) {
			std::cerr << "did not connect nodes" << std::endl;
			if (vm.count("quit")) {
				return 1;
			}
		}
	}

	ear::AudioIoDeviceFunctorCallback callback([&graph] (const float* const*, int, float* const* outputs, int outputChannels, int samples) {
		DBG("callback channels=" + juce::String(outputChannels) + " samples=" + juce::String(samples));

		juce::AudioSampleBuffer buffer(outputs, outputChannels, samples);
		juce::AudioSourceChannelInfo info(buffer);
		juce::MidiBuffer midi;
		graph.processBlock(buffer, midi);
	});

	graph.prepareToPlay(44100, 512);

	//juceSource->prepareToPlay(44100, 512);
	if (!device->addCallback(&callback)) {
		std::cerr << "Failed to register callback" << std::endl;
		return 1;
	}
/*
	juce::AudioSourcePlayer player;
	if (!device->addCallback(&player)) {
		std::cerr << "Failed to register player" << std::endl;
		return 1;
	}
*/
	DBG("sleeping");

	std::this_thread::sleep_for(std::chrono::seconds(5));

	DBG("done sleeping");
//	player->setProcessor(nullptr);
	device->close();

	juce::Logger::writeToLog("done");

	return 0;
}
#endif
int main(int argc, char** argv) {
	boost::program_options::options_description desc("Allowed options");
	desc.add_options()
		("device,d", boost::program_options::value<std::string>(), "Device")
    	("help,h", "produce help message")
		("quit", "Quit on error")
		("connect", "Connect nodes")
		("channel", boost::program_options::value<int>()->default_value(0), "Channel")
		("gain", boost::program_options::value<float>()->default_value(-6.0), "Gain")
		("ramp", boost::program_options::value<double>()->default_value(1.0), "Ramp in seconds")
		("config", boost::program_options::value<std::string>()->default_value("ear.json"), "Config file path")
    	;

	boost::program_options::variables_map vm;

	try {
		boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
		boost::program_options::notify(vm);
	} catch (const boost::program_options::invalid_command_line_syntax& err) {
		std::cerr << "Error while parsing the command line:"
			<< std::endl << std::endl
			<< err.what()
			<< std::endl << std::endl
			<< desc << std::endl;

		return 1;
	}

	if (vm.count("help")) {
		std::cout << desc << "\n";
		return 0;
	}

	DBG("config file: " + juce::String(vm["config"].as<std::string>()));

	nlohmann::json configJson;

	auto configFile = juce::File::getCurrentWorkingDirectory().getChildFile(vm["config"].as<std::string>());
	//juce::File configFile(vm["config"].as<std::string>());
	if (configFile.exists()) {
		DBG("file exists");
		std::string configFileContents(configFile.loadFileAsString().getCharPointer());

		configJson = nlohmann::json::parse(configFileContents, nullptr, false);
		if (configJson == nlohmann::json::value_t::discarded) {
			configJson = {};
		}
	}

	DBG("config file contents: " + juce::String(configJson.dump()));

	DBG("device count" + juce::String(vm.count("device")));
	if (vm.count("device")) {
		configJson["device"] = vm["device"].as<std::string>();
	}

	DBG("config file contents after cli: " + juce::String(configJson.dump()));


	//return pipeTest(vm);
	return run(configJson);
	//return customGraphTest(vm);
	//return graphTest(vm);
	//return simpleTest(vm);
}
