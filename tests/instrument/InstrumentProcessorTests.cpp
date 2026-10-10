#include <composer/instrument/InstrumentProcessor.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

namespace
{

using composer::instrument::InstrumentProcessor;

/** Fills every sample with NaN, so a sample the processor leaves unwritten cannot pass. */
void fillWithSentinel(juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        juce::FloatVectorOperations::fill(buffer.getWritePointer(channel),
                                          std::numeric_limits<float>::quiet_NaN(),
                                          buffer.getNumSamples());
}

bool allSamplesFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const auto* samples = buffer.getReadPointer(channel);

        for (int index = 0; index < buffer.getNumSamples(); ++index)
            if (! std::isfinite(samples[index]))
                return false;
    }

    return true;
}

} // namespace

TEST_CASE("The instrument takes MIDI and renders one stereo output bus")
{
    InstrumentProcessor processor;

    CHECK(processor.getName() == composer::instrument::productName);
    CHECK(processor.acceptsMidi());
    CHECK_FALSE(processor.producesMidi());
    CHECK_FALSE(processor.isMidiEffect());
    CHECK(processor.getTotalNumInputChannels() == 0);
    CHECK(processor.getTotalNumOutputChannels() == 2);
    CHECK(processor.getBusCount(true) == 0);
    CHECK(processor.getBusCount(false) == 1);
}

TEST_CASE("The instrument accepts only a stereo output without inputs")
{
    InstrumentProcessor processor;

    juce::AudioProcessor::BusesLayout stereo;
    stereo.outputBuses.add(juce::AudioChannelSet::stereo());
    CHECK(processor.checkBusesLayoutSupported(stereo));
    CHECK(processor.isBusesLayoutSupported(stereo));

    juce::AudioProcessor::BusesLayout mono;
    mono.outputBuses.add(juce::AudioChannelSet::mono());
    CHECK_FALSE(processor.checkBusesLayoutSupported(mono));
    CHECK_FALSE(processor.isBusesLayoutSupported(mono));

    juce::AudioProcessor::BusesLayout disabledOutput;
    disabledOutput.outputBuses.add(juce::AudioChannelSet::disabled());
    CHECK_FALSE(processor.isBusesLayoutSupported(disabledOutput));

    // A host cannot add a bus the processor does not declare, so the processor's own
    // policy is checked directly as well.
    juce::AudioProcessor::BusesLayout withInput = stereo;
    withInput.inputBuses.add(juce::AudioChannelSet::stereo());
    CHECK_FALSE(processor.checkBusesLayoutSupported(withInput));
    CHECK_FALSE(processor.isBusesLayoutSupported(withInput));
}

TEST_CASE("The instrument writes every output sample across sample-rate changes")
{
    InstrumentProcessor processor;

    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        constexpr int blockSize = 128;
        processor.setRateAndBufferSizeDetails(sampleRate, blockSize);
        processor.prepareToPlay(sampleRate, blockSize);

        juce::AudioBuffer<float> buffer(2, blockSize);
        fillWithSentinel(buffer);

        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), blockSize / 2);
        processor.processBlock(buffer, midi);

        CHECK(allSamplesFinite(buffer));
        CHECK(buffer.getMagnitude(0, blockSize) <= 1.0f);

        fillWithSentinel(buffer);
        midi.clear();
        midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        processor.processBlock(buffer, midi);

        CHECK(allSamplesFinite(buffer));
        processor.releaseResources();
    }
}

TEST_CASE("The instrument offers one named program")
{
    InstrumentProcessor processor;
    CHECK(processor.getNumPrograms() == 1);
    CHECK(processor.getCurrentProgram() == 0);
    CHECK(processor.getProgramName(0) == "Default");
}
