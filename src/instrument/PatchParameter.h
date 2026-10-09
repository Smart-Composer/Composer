#pragma once

#include <composer/instrument/synth/PatchState.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstddef>

namespace composer::instrument
{

/** The host-facing parameter for one patch field.

    Its ID, name, unit, range, default and choices come from contracts::parameterDescriptors, and
    its value lives in the shared synth::PatchState, so the host, the editing thread and the audio
    thread all see one exact value. The ID and curve of every parameter are persistent
    automation identifiers.
*/
class PatchParameter final : public juce::AudioProcessorParameterWithID
{
public:
    PatchParameter(synth::PatchState& state, std::size_t index);

    float getValue() const override;
    void setValue(float newValue) override;
    float getDefaultValue() const override;
    float getValueForText(const juce::String& text) const override;
    juce::String getText(float normalisedValue, int maximumStringLength) const override;
    bool isDiscrete() const override;
    int getNumSteps() const override;

    std::size_t patchIndex() const noexcept
    {
        return index;
    }

private:
    // JUCE destroys parameters after the processor's own members, so nothing here may touch
    // state on destruction.
    synth::PatchState& state;
    std::size_t index;
};

} // namespace composer::instrument
