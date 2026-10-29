#ifndef EAR_TEST_VIRTUAL_AUDIO_DEVICE_HPP
#define EAR_TEST_VIRTUAL_AUDIO_DEVICE_HPP

// A test-only audio device: no OS audio device is opened, started or enumerated.
// The test thread is the device clock: renderBlocks() calls the registered callback
// synchronously, one block at a time, and keeps everything written per channel.

#include "juce_audio_devices/juce_audio_devices.h"

#include <utility>
#include <vector>

namespace ear::test {

class VirtualAudioDevice : public juce::AudioIODevice {
public:
	// Written into every output buffer before each callback, so a channel the callback
	// leaves untouched is told apart from one it writes as silence.
	static constexpr float UNWRITTEN = 12345.0f;

	VirtualAudioDevice(const juce::String& name, const juce::String& typeName, int outputChannels)
		: juce::AudioIODevice(name, typeName)
		, _outputChannels(outputChannels)
	{}

	~VirtualAudioDevice() override { close(); }

	juce::StringArray getOutputChannelNames() override {
		juce::StringArray names;
		for (int channel = 0; channel < _outputChannels; ++channel) {
			names.add("Out " + juce::String(channel));
		}
		return names;
	}

	juce::StringArray getInputChannelNames() override { return {}; }

	juce::Array<double> getAvailableSampleRates() override { return { SAMPLE_RATE }; }

	juce::Array<int> getAvailableBufferSizes() override { return { BLOCK_SIZE }; }

	int getDefaultBufferSize() override { return BLOCK_SIZE; }

	juce::String open(const juce::BigInteger& inputs, const juce::BigInteger& outputs, double sampleRate, int bufferSize) override {
		if (sampleRate != SAMPLE_RATE || bufferSize != BLOCK_SIZE) {
			return "VirtualAudioDevice supports only 44100 Hz and 512-sample blocks";
		}
		_activeInputs = inputs;
		_activeOutputs = outputs;
		_open = true;
		return {};
	}

	void close() override {
		stop();
		_open = false;
	}

	bool isOpen() override { return _open; }

	// Like a real device: announce the start, then stream until stop().
	void start(juce::AudioIODeviceCallback* callback) override {
		if (!_open || callback == nullptr || _callback != nullptr) {
			return;
		}
		_callback = callback;
		_callback->audioDeviceAboutToStart(this);
	}

	void stop() override {
		if (auto* callback = std::exchange(_callback, nullptr)) {
			callback->audioDeviceStopped();
		}
	}

	bool isPlaying() override { return _callback != nullptr; }

	juce::String getLastError() override { return {}; }

	int getCurrentBufferSizeSamples() override { return BLOCK_SIZE; }

	double getCurrentSampleRate() override { return SAMPLE_RATE; }

	int getCurrentBitDepth() override { return 32; }

	juce::BigInteger getActiveOutputChannels() const override { return _activeOutputs; }

	juce::BigInteger getActiveInputChannels() const override { return _activeInputs; }

	int getOutputLatencyInSamples() override { return 0; }

	int getInputLatencyInSamples() override { return 0; }

	// Test API ------------------------------------------------------------------

	// Runs `blocks` device callbacks on the calling thread and appends each block's output
	// to the per-channel recording. Returns false if the device is not started.
	bool renderBlocks(int blocks) {
		if (_callback == nullptr) {
			return false;
		}

		_recorded.resize((size_t) _outputChannels);
		juce::AudioBuffer<float> output(_outputChannels, BLOCK_SIZE);

		for (int block = 0; block < blocks; ++block) {
			for (int channel = 0; channel < _outputChannels; ++channel) {
				juce::FloatVectorOperations::fill(output.getWritePointer(channel), UNWRITTEN, BLOCK_SIZE);
			}

			_callback->audioDeviceIOCallbackWithContext(nullptr, 0, output.getArrayOfWritePointers(),
				_outputChannels, BLOCK_SIZE, {});

			for (int channel = 0; channel < _outputChannels; ++channel) {
				const float* samples = output.getReadPointer(channel);
				_recorded[(size_t) channel].insert(_recorded[(size_t) channel].end(), samples, samples + BLOCK_SIZE);
			}
		}

		return true;
	}

	// Everything written to `channel` since the last clearRecording().
	const std::vector<float>& recorded(int channel) const { return _recorded.at((size_t) channel); }

	void clearRecording() { _recorded.clear(); }

	static constexpr double SAMPLE_RATE = 44100;
	static constexpr int BLOCK_SIZE = 512;

private:
	const int _outputChannels;
	juce::BigInteger _activeInputs;
	juce::BigInteger _activeOutputs;
	bool _open{false};
	juce::AudioIODeviceCallback* _callback{nullptr};
	std::vector<std::vector<float>> _recorded;
};

// Creates only VirtualAudioDevices; never scans for OS devices.
class VirtualAudioDeviceType : public juce::AudioIODeviceType {
public:
	static constexpr const char* TYPE_NAME = "Virtual";

	VirtualAudioDeviceType(int outputChannels)
		: juce::AudioIODeviceType(TYPE_NAME)
		, _outputChannels(outputChannels)
	{}

	void scanForDevices() override {}

	juce::StringArray getDeviceNames(bool wantInputNames = false) const override {
		return wantInputNames ? juce::StringArray{} : juce::StringArray{ DEVICE_NAME };
	}

	int getDefaultDeviceIndex(bool) const override { return 0; }

	int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override {
		return device != nullptr && !asInput && device->getName() == DEVICE_NAME ? 0 : -1;
	}

	bool hasSeparateInputsAndOutputs() const override { return false; }

	juce::AudioIODevice* createDevice(const juce::String& outputDeviceName, const juce::String&) override {
		return new VirtualAudioDevice(outputDeviceName, TYPE_NAME, _outputChannels);
	}

	static constexpr const char* DEVICE_NAME = "Virtual Output";

private:
	const int _outputChannels;
};

} // namespace ear::test

#endif
