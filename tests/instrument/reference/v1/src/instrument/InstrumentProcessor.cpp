#include <composer_v1/instrument/InstrumentProcessor.h>

#include "PatchParameter.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace composer_v1::instrument
{

InstrumentProcessor::InstrumentProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    for (std::size_t index = 0; index < synth::parameterCount; ++index)
    {
        auto parameter = std::make_unique<PatchParameter>(patchState, index);
        parameters[index] = parameter.get();
        addParameter(parameter.release());
    }
}

InstrumentProcessor::~InstrumentProcessor() = default;

std::optional<contracts::ContractError> InstrumentProcessor::applyPatch(const contracts::InstrumentPatch& patch)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (auto error = contracts::validatePatch(patch))
        return error;

    notifyHost(patchState.publish(patch), true);
    return std::nullopt;
}

contracts::InstrumentPatch InstrumentProcessor::currentPatch() const
{
    JUCE_ASSERT_MESSAGE_THREAD

    return patchState.snapshot();
}

void InstrumentProcessor::panic() noexcept
{
    stopRequests.fetch_or(fadeOutRequest, std::memory_order_release);
}

void InstrumentProcessor::notifyHost(std::bitset<synth::parameterCount> changed, bool withGestures)
{
    for (std::size_t index = 0; index < synth::parameterCount; ++index)
    {
        if (! changed.test(index))
            continue;

        auto* parameter = parameters[index];

        if (withGestures)
            parameter->beginChangeGesture();

        // Report the value the parameter holds now, without writing it, so a host write that
        // lands meanwhile is kept and the host hears the latest value.
        parameter->sendValueChangedMessageToListeners(parameter->getValue());

        if (withGestures)
            parameter->endChangeGesture();
    }
}

void InstrumentProcessor::prepareToPlay(double sampleRate, int)
{
    synthesiser.prepare(sampleRate, patchState.snapshot());
    stopRequests.store(0, std::memory_order_relaxed);
}

void InstrumentProcessor::releaseResources()
{
}

void InstrumentProcessor::reset()
{
    stopRequests.fetch_or(resetRequest, std::memory_order_release);
}

void InstrumentProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    renderBlock(buffer, midiMessages, false);
}

void InstrumentProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    renderBlock(buffer, midiMessages, true);
}

void InstrumentProcessor::renderBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages, bool bypassed) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (! synthesiser.isPrepared())
    {
        buffer.clear();
        midiMessages.clear();
        return;
    }

    if (contracts::InstrumentPatch targets; patchState.tryRead(targets))
        synthesiser.setTargets(targets);

    if (const auto requests = stopRequests.exchange(0, std::memory_order_acq_rel); (requests & resetRequest) != 0)
        synthesiser.stopAllVoicesNow();
    else if ((requests & fadeOutRequest) != 0)
        synthesiser.fadeOutAllVoices();

    float* output = numChannels > 0 ? buffer.getWritePointer(0) : nullptr;
    int rendered = 0;

    // Bypass fades every note out and ignores MIDI, so no note is left hanging across it.
    if (bypassed)
    {
        synthesiser.fadeOutAllVoices();
    }
    else
    {
        for (const auto metadata : midiMessages)
        {
            const int position = std::clamp(metadata.samplePosition, rendered, numSamples);

            if (position > rendered)
            {
                synthesiser.render(output != nullptr ? output + rendered : nullptr, position - rendered);
                rendered = position;
            }

            synthesiser.handleMidi(metadata.data, metadata.numBytes);
        }
    }

    if (rendered < numSamples)
        synthesiser.render(output != nullptr ? output + rendered : nullptr, numSamples - rendered);

    for (int channel = 1; channel < numChannels; ++channel)
        juce::FloatVectorOperations::copy(buffer.getWritePointer(channel), buffer.getReadPointer(0), numSamples);

    midiMessages.clear();
}

bool InstrumentProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.inputBuses.isEmpty()
        && layouts.outputBuses.size() == 1
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

const juce::String InstrumentProcessor::getName() const
{
    return productName;
}

bool InstrumentProcessor::acceptsMidi() const
{
    return true;
}

bool InstrumentProcessor::producesMidi() const
{
    return false;
}

bool InstrumentProcessor::isMidiEffect() const
{
    return false;
}

double InstrumentProcessor::getTailLengthSeconds() const
{
    return synth::tailSeconds;
}

bool InstrumentProcessor::hasEditor() const
{
    return false;
}

juce::AudioProcessorEditor* InstrumentProcessor::createEditor()
{
    return nullptr;
}

int InstrumentProcessor::getNumPrograms()
{
    return 1;
}

int InstrumentProcessor::getCurrentProgram()
{
    return 0;
}

void InstrumentProcessor::setCurrentProgram(int)
{
}

const juce::String InstrumentProcessor::getProgramName(int)
{
    // Hosts list the single program by name; VST3 validation requires one.
    return "Default";
}

void InstrumentProcessor::changeProgramName(int, const juce::String&)
{
}

void InstrumentProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    try
    {
        const auto encoded = contracts::encodePatch(patchState.snapshot());

        if (const auto* text = std::get_if<std::string>(&encoded))
        {
            destData.replaceAll(text->data(), text->size());
            return;
        }
    }
    catch (...)
    {
    }

    destData.reset();
}

void InstrumentProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0 || sizeInBytes > maximumStateBytes)
        return;

    try
    {
        const auto decoded = contracts::decodePatch(
            std::string_view(static_cast<const char*>(data), static_cast<std::size_t>(sizeInBytes)));

        // A state that fails to decode is ignored, so a corrupt session cannot silently reset
        // the instrument.
        if (const auto* patch = std::get_if<contracts::InstrumentPatch>(&decoded))
            notifyHost(patchState.publish(*patch), false);
    }
    catch (...)
    {
    }
}

} // namespace composer_v1::instrument
