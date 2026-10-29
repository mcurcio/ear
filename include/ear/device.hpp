#include <cassert>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <algorithm>


#include "juce_core/juce_core.h"
#include "juce_audio_devices/juce_audio_devices.h"
#include "juce_audio_basics/juce_audio_basics.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/name_generator.hpp>

#ifndef EAR_DEVICE_HPP
#define EAR_DEVICE_HPP

namespace ear {

/*
class RawAudioSource : public AudioSource {
	juce::AudioSource* _source{nullptr};

public:
	RawAudioSource(juce::AudioSource* source)
	: _source(source)
	{}

	int getChannelCount() const {
		return 0;
	}

	void releaseResources() {
		_source->releaseResources();
	}

	void prepareToPlay(int samplesPerBlockExpected, double sampleRate) {
		_source->prepareToPlay(samplesPerBlockExpected, sampleRate);
	}

	void getNextAudioBlock(const AudioSourceChannelInfo &bufferToFill) {
		_source->getNextAudioBlock(bufferToFill);
	}
};

class AudioDeviceSink {
public:
    juce::CriticalSection _mutex;
    std::unique_ptr<AudioSource> _source{nullptr};
    juce::AudioSourcePlayer _player;


    void setSource(std::unique_ptr<AudioSource>&& source) {
        juce::ScopedLock lock(_mutex);
        _source = std::move(source);
        _player.setSource(_source.get());
    }
    void update(float** data, int numChannels, int numSamples) {
        //DBG("AudioDeviceSink::update");
        _player.audioDeviceIOCallback(NULL, 0, data, numChannels, numSamples);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) {
        _player.audioDeviceAboutToStart(device);
    }

    void audioDeviceStopped() {
        _player.audioDeviceStopped();
    }
};
*/

class AudioIoDevice : public juce::AudioIODeviceCallback {
    std::shared_ptr<juce::AudioIODevice> _device{nullptr};
    std::shared_ptr<juce::AudioIODeviceType> _type{nullptr};

    juce::CriticalSection _mutex;
	uint64_t _count{0};
	std::vector<juce::AudioIODeviceCallback*> _callbacks;
	juce::AudioIODevice* _startedDevice{nullptr};

    //std::shared_ptr<AudioDeviceSource> _input;
    //std::shared_ptr<AudioDeviceSink> _output;
	juce::AudioBuffer<float> _outputCache;

	juce::AudioSampleBuffer _buffer;

public:
    static juce::Array<std::shared_ptr<juce::AudioIODeviceType>> getAllDeviceTypes() {
        juce::AudioDeviceManager manager;

        juce::OwnedArray<juce::AudioIODeviceType> types;
        manager.createAudioDeviceTypes(types);

        juce::Array<std::shared_ptr<juce::AudioIODeviceType>> typeArray;

        while (!types.isEmpty()) {
            std::shared_ptr<juce::AudioIODeviceType> type(types.removeAndReturn(0));
            typeArray.add(std::move(type));
        }

        return typeArray;
    }

    static juce::Array<std::shared_ptr<AudioIoDevice>> getAllDevices() {
        juce::Array<std::shared_ptr<AudioIoDevice>> devices;

        for (auto& type: getAllDeviceTypes()) {
            type->scanForDevices();

            juce::StringArray deviceNames(type->getDeviceNames());

            for (juce::String& name : deviceNames) {
                auto d = std::make_shared<AudioIoDevice>(type, name);
                devices.add(d);
            }
        }

        return devices;
    }

	static std::shared_ptr<AudioIoDevice> getDevice(const boost::uuids::uuid& uuid) {
		for (auto& device : getAllDevices()) {
			if (device->getUuid() == uuid) {
				return device;
			}
		}

		return nullptr;
	}

    AudioIoDevice(std::shared_ptr<juce::AudioIODeviceType>& type, juce::String name)
    : _device(type->createDevice(name, name))
    , _type(type)
    {}

    ~AudioIoDevice() {
        close();
    }

	std::shared_ptr<juce::AudioIODevice> getDevice() const { return _device; }

	std::shared_ptr<juce::AudioIODeviceType> getDeviceType() const { return _type; }

    juce::String getName() const { return _device->getName(); }

    juce::String getTypeName() const { return _device->getTypeName(); }

	boost::uuids::uuid getUuid() const {
		boost::uuids::uuid nil{};
		assert(nil.is_nil());
		boost::uuids::name_generator generator(nil);
		return generator((getTypeName() + ":" + getName()).toRawUTF8());
	}

    juce::StringArray getOutputChannelNames() const { return _device->getOutputChannelNames(); }

    unsigned getOutputChannelCount() const { return _device->getOutputChannelNames().size(); }

    unsigned getInputChannelCount() const { return _device->getInputChannelNames().size(); }

	juce::Array<double> getAvailableSampleRates() const {
		return _device->getAvailableSampleRates();
	}

	juce::Array<int> getAvailableBufferSizes() const {
		return _device->getAvailableBufferSizes();
	}

	int getDefaultBufferSize() const { return _device->getDefaultBufferSize(); }

	bool addCallback(juce::AudioIODeviceCallback* callback) {
		juce::ScopedLock lock(_mutex);

		auto it = std::find(_callbacks.begin(), _callbacks.end(), callback);
		if (it != _callbacks.end()) {
			// already in list
			return false;
		}

		// if already started, send callback
		if (_startedDevice != nullptr) {
			callback->audioDeviceAboutToStart(_startedDevice);
		}

		_callbacks.push_back(callback);
		return true;
	}

	bool removeCallback(juce::AudioIODeviceCallback* callback) {
		juce::ScopedLock lock(_mutex);

		auto it = std::remove(_callbacks.begin(), _callbacks.end(), callback);
		if (it == _callbacks.end()) {
			// none found
			return false;
		}

		// if in the "running" state, then send stop signal
		if (_startedDevice != nullptr) {
			for (auto localIt = it; it != _callbacks.end(); ++localIt) {
				(*localIt)->audioDeviceStopped();
			}
		}

		_callbacks.erase(it, _callbacks.end());
		return true;
	}

    bool open() {
        juce::BigInteger inputs;
        inputs.setRange(0, getInputChannelCount(), true);

        juce::BigInteger outputs;
        outputs.setRange(0, getOutputChannelCount(), true);

		double rate = 44100;
		const auto rates = getAvailableSampleRates();
		if (!rates.contains(rate)) {
			rate = rates[0];
		}

        juce::String err = _device->open(inputs, outputs, rate, 512);

        if (!err.isEmpty()) {
			DBG("Device open error=" << err);
        }

        //_input = std::make_shared<AudioDeviceSource>();
        //_output = std::make_shared<AudioDeviceSink>();

        return err == "";
    }

    void close() {
        _device->close();

        //_input = nullptr;
        //_output = nullptr;
    }

    void start() {
        DBG("starting device");
	    _device->start(this);
    }

    void stop() {
        _device->stop();
    }

    //std::shared_ptr<AudioDeviceSource> getInputDevice() const { return _input; }

    //std::shared_ptr<AudioDeviceSink> getOutputDevice() const { return _output; }

	void outputBuffer(juce::AudioBuffer<float>& buffer) {
		DBG("outputBuffer");
		juce::ScopedLock lock(_mutex);
		_outputCache.makeCopyOf(buffer);
	}

	bool operator==(const AudioIoDevice& rhs) const {
		return getUuid() == rhs.getUuid();
	}

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData, int numOutputChannels, int requestedSamples, const juce::AudioIODeviceCallbackContext& context) override {
		const juce::ScopedLock lock(_mutex);

        //DBG("audioDeviceIOCallback frame=" + juce::String(_count) + ", callbacks=" + juce::String(_callbacks.size()));

		++_count;

		int callback = 0;
		for (auto& cb : _callbacks) {
			//DBG(juce::String("    * callback ") + juce::String(callback++));
			cb->audioDeviceIOCallbackWithContext(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, requestedSamples, context);
		}
		//DBG(juce::String("~audioDeviceIOCallback [0][0]=") << outputChannelData[0][0] << ", [1][0]=" << outputChannelData[1][0]);
/*
		if (_graph) {
			_buffer.setSize(numOutputChannels, requestedSamples);

//			juce::AudioBuffer<float> buffer(outputChannelData, numOutputChannels, requestedSamples);
			juce::MidiBuffer midi;

			//buffer.setSample(0, 0, 0.01);
			DBG("before processBlock sample[0][0]=" + juce::String(_buffer.getSample(0, 0)));
			_graph->processBlock(_buffer, midi);

			DBG("audioDeviceIOCallback graph#processBlock numOutputChannels=" + juce::String(numOutputChannels) + " numSamples=" + juce::String(requestedSamples) + " [0][0]=" + juce::String(_buffer.getSample(0, 0)));
		}

		int numChannels = std::min(_outputCache.getNumChannels(), numOutputChannels);
		int numSamples = std::min(_outputCache.getNumSamples(), requestedSamples);
		for (int i = 0; i < numChannels; ++i) {
			memcpy(outputChannelData[i], _outputCache.getReadPointer(i), numSamples);
		}

        //_input->update(inputChannelData, numInputChannels, numSamples);
        //_output->update(outputChannelData, numOutputChannels, numSamples);

		DBG("~audioDeviceIOCallback [0][0]=" + juce::String(_outputCache.getSample(0, 0)));
*/
    }


    void audioDeviceAboutToStart(juce::AudioIODevice *device) override {
        DBG("audioDeviceAboutToStart");

		const juce::ScopedLock lock(_mutex);

		for (auto& cb : _callbacks) {
			cb->audioDeviceAboutToStart(device);
		}

		_startedDevice = device;
/*
		auto newSampleRate = device->getCurrentSampleRate();
		auto newBlockSize  = device->getCurrentBufferSizeSamples();
		auto numChansIn    = device->getActiveInputChannels().countNumberOfSetBits();
		auto numChansOut   = device->getActiveOutputChannels().countNumberOfSetBits();

		juce::ScopedLock lock(_mutex);

		_graph->prepareToPlay(newSampleRate, newBlockSize);
        //_input->audioDeviceAboutToStart(device);
        //_output->audioDeviceAboutToStart(device);
*/
    }

    void audioDeviceStopped() override {
        DBG("audioDeviceStopped");

		const juce::ScopedLock lock(_mutex);

		for (auto& cb : _callbacks) {
			cb->audioDeviceStopped();
		}

		_startedDevice = nullptr;

        //_input->audioDeviceStopped();
        //_output->audioDeviceStopped();
    }

    void audioDeviceError(const juce::String &errorMessage) override {
        juce::Logger::writeToLog("audioDeviceError=" + errorMessage);

		const juce::ScopedLock lock(_mutex);

		for (auto& cb : _callbacks) {
			cb->audioDeviceError(errorMessage);
		}
    }
};

class AudioIoDeviceCallback : public juce::AudioIODeviceCallback {
public:
	virtual ~AudioIoDeviceCallback() = default;

	virtual void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int, const juce::AudioIODeviceCallbackContext&) override {}

	virtual void audioDeviceAboutToStart(juce::AudioIODevice*) override {}

	virtual void audioDeviceStopped() override {}

	virtual void audioDeviceError(const juce::String&) override {}
};

class AudioIoDeviceFunctorCallback : public AudioIoDeviceCallback {
	using Function = std::function<void (const float* const*, int, float* const*, int, int)>;
private:
	Function _fn{nullptr};

public:
	AudioIoDeviceFunctorCallback(Function fn)
		: _fn(fn)
	{}

	void audioDeviceIOCallbackWithContext(const float* const* inputs, int inputChannels, float* const* outputs, int outputChannels, int samples, const juce::AudioIODeviceCallbackContext&) override {
		_fn(inputs, inputChannels, outputs, outputChannels, samples);
	}
};

} // namespace ear

#endif
