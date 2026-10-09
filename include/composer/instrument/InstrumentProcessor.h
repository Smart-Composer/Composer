#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace composer::instrument
{

/** The instrument's product name. The plugin's PRODUCT_NAME in src/plugin/CMakeLists.txt must
    match it; the hosting tests compare the two. */
inline constexpr auto productName = "Composer Instrument";

/** The instrument processor shared by the Composer application and the VST3 plugin.

    It takes MIDI input and renders to one stereo output bus, without audio inputs.
    The output is silent until the shared synth voice is added.
*/
class InstrumentProcessor final : public juce::AudioProcessor
{
public:
    InstrumentProcessor();
    ~InstrumentProcessor() override;

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    using juce::AudioProcessor::processBlock;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    bool hasEditor() const override;
    juce::AudioProcessorEditor* createEditor() override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentProcessor)
};

} // namespace composer::instrument
