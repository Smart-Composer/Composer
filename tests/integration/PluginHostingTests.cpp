#include <composer/instrument/InstrumentProcessor.h>

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <limits>

namespace
{

/** The .vst3 bundle directory around the module CMake built for this configuration. */
juce::File builtPluginBundle()
{
    // <bundle>.vst3/Contents/x86_64-win/<module>.vst3
    return juce::File(COMPOSER_TEST_VST3_MODULE).getParentDirectory().getParentDirectory().getParentDirectory();
}

std::unique_ptr<juce::PluginDescription> describeBuiltPlugin(juce::AudioPluginFormat& format)
{
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, builtPluginBundle().getFullPathName());

    REQUIRE(descriptions.size() == 1);
    return std::unique_ptr<juce::PluginDescription>(descriptions.removeAndReturn(0));
}

/** Fills every sample with NaN, so a sample the plugin leaves unwritten cannot pass. */
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

TEST_CASE("Scanning the built VST3 bundle finds the Composer instrument")
{
    const auto bundle = builtPluginBundle();
    REQUIRE(bundle.isDirectory());
    CHECK(bundle.getFileExtension() == ".vst3");

    // A scan reads the bundle's manifest; bus layouts are only known once instantiated.
    juce::VST3PluginFormat format;
    const auto description = describeBuiltPlugin(format);

    CHECK(description->name == composer::instrument::productName);
    CHECK(description->manufacturerName == "Composer");
    CHECK(description->pluginFormatName == "VST3");
    CHECK(description->isInstrument);
    CHECK(juce::File(description->fileOrIdentifier) == bundle);
}

TEST_CASE("A JUCE host instantiates the built VST3 and processes MIDI blocks")
{
    juce::VST3PluginFormat format;
    const auto description = describeBuiltPlugin(format);

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 128;

    juce::String error;
    auto instance = format.createInstanceFromDescription(*description, sampleRate, blockSize, error);
    INFO(error.toStdString());
    REQUIRE(instance != nullptr);

    CHECK(instance->getName() == composer::instrument::productName);
    CHECK(instance->acceptsMidi());
    CHECK(instance->getTotalNumInputChannels() == 0);
    CHECK(instance->getTotalNumOutputChannels() == 2);

    const auto live = instance->getPluginDescription();
    CHECK(live.isInstrument);
    CHECK(live.numInputChannels == 0);
    CHECK(live.numOutputChannels == 2);

    instance->setRateAndBufferSizeDetails(sampleRate, blockSize);
    instance->prepareToPlay(sampleRate, blockSize);

    juce::AudioBuffer<float> buffer(2, blockSize);

    for (int block = 0; block < 64; ++block)
    {
        fillWithSentinel(buffer);
        juce::MidiBuffer midi;

        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);

        if (block == 32)
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), blockSize / 2);

        instance->processBlock(buffer, midi);
        REQUIRE(allSamplesFinite(buffer));
    }

    instance->releaseResources();
}
