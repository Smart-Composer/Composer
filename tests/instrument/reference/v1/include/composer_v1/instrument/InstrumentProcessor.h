#pragma once

#include <composer_v1/contracts/InstrumentPatch.h>
#include <composer_v1/instrument/synth/PatchState.h>
#include <composer_v1/instrument/synth/Synth.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <optional>

namespace composer_v1::instrument
{

/** The instrument's product name. The plugin's PRODUCT_NAME in src/plugin/CMakeLists.txt must
    match it; the hosting tests compare the two. */
inline constexpr auto productName = "Composer Instrument";

class PatchParameter;

/** The instrument processor shared by the Composer application and the VST3 plugin.

    It takes MIDI input and renders the shared synthesiser to one stereo output bus, without
    audio inputs. Its host parameters mirror the patch fields, and its state is exactly the
    patch's contract JSON. Patch changes and host automation take effect at the start of a
    processed block: normally the next, or a later one if that block's read overlaps a change.

    Threads: applyPatch and currentPatch belong to the message thread; panic may be called from
    any thread; processing never blocks, allocates or validates.
*/
class InstrumentProcessor final : public juce::AudioProcessor
{
public:
    /** The largest state setStateInformation accepts. */
    static constexpr int maximumStateBytes = 64 * 1024;

    InstrumentProcessor();
    ~InstrumentProcessor() override;

    /** Plays a patch from the next processed block. Gain, sustain, cutoff and resonance ramp
        over 20 ms; a waveform change applies to notes started afterwards. Message thread only.
        Returns the validation error, changing nothing, when the patch is invalid. */
    [[nodiscard]] std::optional<contracts::ContractError> applyPatch(const contracts::InstrumentPatch& patch);

    /** The patch being played: exactly the last applied patch until the host automates a
        parameter or a state restore replaces it. Message thread only. */
    [[nodiscard]] contracts::InstrumentPatch currentPatch() const;

    /** Fades every note out over 3 ms from the start of the next processed block, with no
        release tail. The patch is unchanged. Any thread; never blocks or allocates. */
    void panic() noexcept;

    /** Silences every voice at once at the start of the next processed block, for a break in
        the audio such as the host stopping processing. Any thread. */
    void reset() override;

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

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
    void renderBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages, bool bypassed) noexcept;
    void notifyHost(std::bitset<synth::parameterCount> changed, bool withGestures);

    synth::PatchState patchState;
    synth::Synth synthesiser;
    std::array<PatchParameter*, synth::parameterCount> parameters {};

    static constexpr std::uint32_t fadeOutRequest = 1;
    static constexpr std::uint32_t resetRequest = 2;
    std::atomic<std::uint32_t> stopRequests { 0 };

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentProcessor)
};

} // namespace composer_v1::instrument
