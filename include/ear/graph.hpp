#include <cassert>

#include "device.hpp"
#include "processor.hpp"

#include "juce_audio_processors_headless/juce_audio_processors_headless.h"

#ifndef EAR_GRAPH_HPP
#define EAR_GRAPH_HPP

namespace ear {

class GraphDeviceSink : public GraphProcessor, public juce::AudioIODeviceCallback {
public:
	std::shared_ptr<AudioIoDevice> _device;

	juce::CriticalSection _mutex;
	juce::AudioSampleBuffer _cache;

	GraphDeviceSink(const std::shared_ptr<AudioIoDevice> &device)
		: GraphProcessor()
		, _device(device)
	{
		_device->addCallback(this);
	}

	virtual ~GraphDeviceSink() {
		_device->removeCallback(this);
	}

	void prepareToPlay(double, int) override {
		//setPlayConfigDetails(1, 2, sampleRate, estimatedSamplesPerBlock);
		//_source->prepareToPlay(sampleRate, estimatedSamplesPerBlock);
	}

	void releaseResources() override {
		//_source->releaseResources();
	}

	void processBlock(juce::AudioSampleBuffer& buffer, juce::MidiBuffer&) override {
		DBG("GraphDeviceSink#processBlock [0][0]=" + juce::String(buffer.getSample(0, 0)));

		juce::ScopedLock lock(_mutex);
		_cache.makeCopyOf(buffer);
	}

private:
	void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int outputChannels, int samples, const juce::AudioIODeviceCallbackContext&) override {
		juce::AudioSampleBuffer buffer(outputs, outputChannels, samples);

		DBG("GraphDeviceSink#audioDeviceIOCallback [0][0]=" + juce::String(buffer.getSample(0, 0)));

		juce::ScopedLock lock(_mutex);
		buffer.makeCopyOf(_cache);
	}

	void audioDeviceAboutToStart(juce::AudioIODevice*) override {}

	void audioDeviceStopped() override {}
};

class AudioGraphNode {
	juce::AudioProcessorGraph::Node::Ptr _nodePtr{nullptr};

public:
	AudioGraphNode() = default;

	AudioGraphNode(juce::AudioProcessorGraph::Node::Ptr ptr)
		: _nodePtr(ptr)
	{}

	juce::AudioProcessorGraph::NodeID getNodeId() const {
		return _nodePtr->nodeID;
	}

	uint32_t id() const {
		return _nodePtr->nodeID.uid;
	}

	int getInputChannelCount() const {
		return _nodePtr->getProcessor()->getTotalNumInputChannels();
	}

	int getOutputChannelCount() const {
		return _nodePtr->getProcessor()->getTotalNumOutputChannels();
	}
};

class AudioGraph : public juce::AudioProcessorGraph {
public:
	class Connection {
	public:
		class ConnectionChannel {
		public:
			AudioGraphNode node;
			int channel;

			ConnectionChannel(AudioGraphNode n, int c)
				: node(n)
				, channel(c)
			{}
		};

		Connection(ConnectionChannel s, ConnectionChannel d)
			: source(s)
			, destination(d)
		{}

		ConnectionChannel source;
		ConnectionChannel destination;

		juce::AudioProcessorGraph::Connection toJuceConnection() const {
			return {
					{source.node.getNodeId(), source.channel},
					{destination.node.getNodeId(), destination.channel}
			};
		}
	};

	AudioGraph() {
		setPlayConfigDetails(0, 0, 44100, 512);
	}

	virtual ~AudioGraph() = default;

	AudioGraphNode addNode(std::unique_ptr<juce::AudioProcessor> processor) {
		auto graphNode = juce::AudioProcessorGraph::addNode(std::move(processor));
		AudioGraphNode node(graphNode);
		DBG("Added node " + juce::String(node.id()));
		return node;
	}

	bool removeNode(AudioGraphNode node) {
		bool removed = juce::AudioProcessorGraph::removeNode(node.getNodeId()) == nullptr;
		DBG(juce::String(removed ? "Succesfully removed" : "Failed to remove") + " node " + juce::String(node.id()));
		return removed;
	}

	bool canConnect(const Connection& conn) {
		return juce::AudioProcessorGraph::canConnect(conn.toJuceConnection());
	}

	bool addConnection(const Connection& conn) {
		bool connected = juce::AudioProcessorGraph::addConnection(conn.toJuceConnection());
		DBG(juce::String(connected?"Succesfully connected ":"Failed to connect ")
			+ juce::String(conn.source.node.id()) + ":" + juce::String(conn.source.channel)
			+ " to "
			+ juce::String(conn.destination.node.id()) + ":" + juce::String(conn.destination.channel));
		return connected;
	}

	bool removeConnection(const Connection& conn) {
		bool disconnected = juce::AudioProcessorGraph::removeConnection(conn.toJuceConnection());
		DBG(juce::String(disconnected?"Succesfully disconnected ":"Failed to disconnect ")
			+ juce::String(conn.source.node.id()) + ":" + juce::String(conn.source.channel)
			+ " from "
			+ juce::String(conn.destination.node.id()) + ":" + juce::String(conn.destination.channel));
		return disconnected;
	}
};

class AudioIoDeviceOutputProcessor : public GraphProcessor, public AudioIoDeviceCallback {
	std::shared_ptr<AudioIoDevice> _device{nullptr};
	unsigned _channels{0};
	juce::AudioSampleBuffer _buffer;

public:
	AudioIoDeviceOutputProcessor(const std::shared_ptr<AudioIoDevice>& device)
		: _device(device)
		, _channels(device->getOutputChannelCount())
		, _buffer(_channels, 512)
	{
		setPlayConfigDetails(_channels, 1, 44100, 512);

		_buffer.clear();

		_device->addCallback(this);
	}

	// no copy
	AudioIoDeviceOutputProcessor(const AudioIoDeviceOutputProcessor&) = delete;

	~AudioIoDeviceOutputProcessor() {
		_device->removeCallback(this);
	}

	void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int channels, int samples, const juce::AudioIODeviceCallbackContext&) override {
		juce::AudioSampleBuffer outputBuffer(outputs, channels, samples);

		assert(outputBuffer.getNumChannels() <= _buffer.getNumChannels());
		assert(outputBuffer.getNumSamples() <= _buffer.getNumSamples());

		float before = outputBuffer.getSample(0, 0);
		//outputBuffer.copyFrom(0, 0, _buffer, 0, 0, samples);
		for (unsigned channel = 0; channel < _channels; ++channel) {
			outputBuffer.copyFrom(channel, 0, _buffer, channel, 0, samples);
		}
		//DBG(juce::String("+ AudioIoDeviceOutputProcessor callback [0][0] before=") << before << ", after=" << outputBuffer.getSample(0, 0));
	}

	void processBlock(juce::AudioSampleBuffer& buffer, juce::MidiBuffer&) override {
		//DBG(juce::String("- AudioIoDeviceOutputProcessor process [0][0]=") << buffer.getSample(0, 0) << " [1][0]=" << buffer.getSample(1, 0));
		for (unsigned channel = 0; channel < _channels; ++channel) {
			_buffer.copyFrom(channel, 0, buffer, channel, 0, buffer.getNumSamples());
		}
	}
};

class AudioGraphStandalone : public AudioGraph, public AudioIoDeviceCallback {
private:

	juce::CriticalSection _mutex;
	std::shared_ptr<AudioIoDevice> _device{nullptr};
	AudioGraphNode _deviceNode{nullptr};
	//unsigned _channelCountCache{0};
	juce::AudioSampleBuffer _buffer{0, 512};
	juce::MidiBuffer _midi;

public:
	AudioGraphNode _inputNode;
	AudioGraphNode _outputNode;

	AudioGraphStandalone() {
				setPlayConfigDetails(0, 0, 44100, 512);

		std::unique_ptr<juce::AudioProcessor> graphInput = std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode);
		_inputNode = addNode(std::move(graphInput));

		std::unique_ptr<juce::AudioProcessor> graphOutput = std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode);
		//graphOutput->setPlayConfigDetails(1, 0, 44100, 512);
		_outputNode = addNode(std::move(graphOutput));
		DBG("output config id=" + juce::String(_outputNode.id()));


	}

	AudioGraphNode setDevice(const std::shared_ptr<AudioIoDevice>& device) {
		juce::ScopedLock lock(_mutex);

		if (device != _device) {
			if (_device != nullptr) {
				// TODO: allow to replace device
				assert(false);
			}

			// create output processor node
			auto processor = std::make_unique<AudioIoDeviceOutputProcessor>(device);
			processor->prepareToPlay(44100, 512);
			_deviceNode = addNode(std::move(processor));

			// connect output processor node to fake output so the graph will process it
			std::cout << "device outputchannels=" << _deviceNode.getOutputChannelCount()
				<< " inputchannels=" << _outputNode.getInputChannelCount() << std::endl;
			//assert(addConnection({{_deviceNode, 0}, {_outputNode, 0}}));

			// create device mapping (which automatically registers the callback)
			_device = device;
			//_buffer.setSize(0/*device->getOutputChannelCount()*/, 512);

			_device->addCallback(this);
		} else {
			// not supported yet
			assert(false);
		}

		return _deviceNode;
	}

private:
	// The device's start and stop events prepare and release the graph, as
	// juce::AudioProcessorPlayer does. A JUCE 9 AudioProcessorGraph that was never prepared
	// builds no render sequence: it renders silence and never prepares its nodes.
	void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
		juce::ScopedLock lock(_mutex);
		prepareToPlay(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
	}

	void audioDeviceStopped() override {
		juce::ScopedLock lock(_mutex);
		releaseResources();
	}

	void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int channels, int samples, const juce::AudioIODeviceCallbackContext&) override {
		juce::ScopedLock lock(_mutex);

		//DBG("");
		//DBG("callback channels=" + juce::String(channels) + " samples=" + juce::String(samples));

		//assert(channels <= _buffer.getNumChannels());
		//assert(samples <= _buffer.getNumSamples());

		//juce::AudioSampleBuffer buffer(outputs, outputChannels, samples);
		//juce::AudioSourceChannelInfo info(buffer);

		_buffer.clear();
		processBlock(_buffer, _midi);

		//DBG(juce::String() << "")
	}
};

} // namespace ear

#endif
