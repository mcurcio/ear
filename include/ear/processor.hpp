#ifndef EAR_PROCESSOR_HPP
#define EAR_PROCESSOR_HPP

#include "sources.hpp"

#include "juce_dsp/juce_dsp.h"
#include "juce_audio_processors/juce_audio_processors.h"

namespace ear {

class GraphProcessor : public juce::AudioProcessor {
public:
	GraphProcessor() = default;
	GraphProcessor(const juce::AudioProcessor::BusesProperties& bus)
		: juce::AudioProcessor(bus)
	{}

	virtual ~GraphProcessor() = default;

    virtual void prepareToPlay(double, int) override {}
    virtual void releaseResources() override {}
    virtual void processBlock(juce::AudioSampleBuffer&, juce::MidiBuffer&) override {}

    virtual juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    virtual bool hasEditor() const override { return false; }

    virtual const juce::String getName() const override  { return {}; }
    virtual bool acceptsMidi() const override { return false; }
    virtual bool producesMidi() const override { return false; }
    virtual double getTailLengthSeconds() const override { return 0; }

    virtual int getNumPrograms() override { return 0; }
    virtual int getCurrentProgram() override { return 0; }
    virtual void setCurrentProgram (int) override {}
    virtual const juce::String getProgramName (int) override { return {}; }
    virtual void changeProgramName (int, const juce::String&) override {}

    virtual void getStateInformation (juce::MemoryBlock&) override {}
    virtual void setStateInformation (const void*, int) override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphProcessor)
};

class GainProcessor : public GraphProcessor {
private:
	juce::dsp::Gain<float> _gain;

public:
	GainProcessor() {
		_gain.setGainDecibels(0);
	}

	~GainProcessor() = default;

	void setGainDecibels(float gain) {
		_gain.setGainDecibels(gain);
	}

	void setRampDurationSeconds(double seconds) {
		_gain.setRampDurationSeconds(seconds);
	}

	void prepareToPlay(double sampleRate, int samplesPerBlock) override {
    	juce::dsp::ProcessSpec spec {sampleRate, static_cast<uint32_t>(samplesPerBlock), 2};
    	_gain.prepare(spec);
	}

	void processBlock(juce::AudioSampleBuffer& buffer, juce::MidiBuffer&) override {
	    juce::dsp::AudioBlock<float> block(buffer);
	    juce::dsp::ProcessContextReplacing<float> context(block);
	    _gain.process(context);
	}

	void reset() override {
		_gain.reset();
	}
};

class GraphSource : public GraphProcessor {
public:
	std::shared_ptr<juce::AudioSource> _source;
	juce::AudioSourcePlayer _player;

	GraphSource(const std::shared_ptr<juce::AudioSource> &source, const juce::AudioProcessor::BusesProperties& bus={})
	 	: GraphProcessor(bus)
		, _source(source)
	{
		_player.setSource(_source.get());
	}

	GraphSource(juce::AudioSource* source, const juce::AudioProcessor::BusesProperties& bus={})
	 	: GraphProcessor(bus)
	{
		_player.setSource(source);
	}

	void prepareToPlay(double sampleRate, int estimatedSamplesPerBlock) override {
		//setPlayConfigDetails(0, 1, sampleRate, estimatedSamplesPerBlock);
		_player.prepareToPlay(sampleRate, estimatedSamplesPerBlock);
	}

	void processBlock(juce::AudioSampleBuffer& buffer, juce::MidiBuffer&) override {
		//DBG("GraphSource#processBlock");
		float input = buffer.getSample(0, 0);
		//juce::AudioSourceChannelInfo info(buffer);
		//_source->getNextAudioBlock(info);
		_player.audioDeviceIOCallbackWithContext(nullptr, 0, buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), {});

		//DBG("~GraphSource#processBlock input=" + juce::String(input) + " output=" + juce::String(buffer.getSample(0, 0)));
	}
};

class WhiteNoiseProcessor : public GraphSource {
private:
	WhiteNoiseSource _whiteNoise;

public:
	WhiteNoiseProcessor()
		: GraphSource(&_whiteNoise, juce::AudioProcessor::BusesProperties().withOutput("main", juce::AudioChannelSet::mono()))
	{}
};

} // ear namespace

#endif
