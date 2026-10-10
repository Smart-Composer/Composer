#include "PatchParameter.h"

namespace composer_v1::instrument
{
namespace
{

const contracts::ParameterDescriptor& descriptorAt(std::size_t index)
{
    return contracts::parameterDescriptors[index];
}

juce::String fromView(std::string_view text)
{
    return juce::String(text.data(), text.size());
}

bool isChoice(std::size_t index)
{
    return descriptorAt(index).kind == contracts::ParameterKind::choice;
}

int decimalPlaces(std::size_t index)
{
    const auto unit = descriptorAt(index).unit;

    if (unit == "dB")
        return 1;

    if (unit == "Hz")
        return 0;

    if (unit == "Q")
        return 2;

    return 3;
}

} // namespace

PatchParameter::PatchParameter(synth::PatchState& patchState, std::size_t parameterIndex)
    : juce::AudioProcessorParameterWithID(
          juce::ParameterID { fromView(descriptorAt(parameterIndex).id), 1 },
          fromView(descriptorAt(parameterIndex).displayName),
          juce::AudioProcessorParameterWithIDAttributes().withLabel(fromView(descriptorAt(parameterIndex).unit))),
      state(patchState),
      index(parameterIndex)
{
}

float PatchParameter::getValue() const
{
    return state.hostValue(index);
}

void PatchParameter::setValue(float newValue)
{
    state.setHostValue(index, newValue);
}

float PatchParameter::getDefaultValue() const
{
    return synth::normalise(index, descriptorAt(index).defaultValue);
}

float PatchParameter::getValueForText(const juce::String& text) const
{
    if (isChoice(index))
    {
        const auto trimmed = text.trim();

        for (std::size_t choice = 0; choice < contracts::waveformDescriptors.size(); ++choice)
        {
            const auto& option = contracts::waveformDescriptors[choice];

            if (trimmed.equalsIgnoreCase(fromView(option.displayName)) || trimmed.equalsIgnoreCase(fromView(option.id)))
                return synth::normalise(index, static_cast<double>(choice));
        }

        return 0.0f;
    }

    return synth::normalise(index, text.getDoubleValue());
}

juce::String PatchParameter::getText(float normalisedValue, int maximumStringLength) const
{
    const double value = synth::denormalise(index, normalisedValue);
    juce::String text;

    if (isChoice(index))
        text = fromView(contracts::waveformDescriptors[static_cast<std::size_t>(value)].displayName);
    else if (const int places = decimalPlaces(index); places > 0)
        text = juce::String(value, places);
    else
        // juce::String treats zero decimal places as "default format", so round whole units.
        text = juce::String(juce::roundToInt(value));

    return maximumStringLength > 0 ? text.substring(0, maximumStringLength) : text;
}

bool PatchParameter::isDiscrete() const
{
    return isChoice(index);
}

int PatchParameter::getNumSteps() const
{
    return isChoice(index) ? static_cast<int>(contracts::waveformDescriptors.size())
                           : juce::AudioProcessor::getDefaultNumParameterSteps();
}

} // namespace composer_v1::instrument
