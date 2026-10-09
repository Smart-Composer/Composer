#include <composer/instrument/InstrumentProcessor.h>

namespace composer::instrument
{

InstrumentProcessor::InstrumentProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

InstrumentProcessor::~InstrumentProcessor() = default;

void InstrumentProcessor::prepareToPlay(double, int)
{
}

void InstrumentProcessor::releaseResources()
{
}

void InstrumentProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
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
    return 0.0;
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
    return {};
}

void InstrumentProcessor::changeProgramName(int, const juce::String&)
{
}

void InstrumentProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    destData.reset();
}

void InstrumentProcessor::setStateInformation(const void*, int)
{
}

} // namespace composer::instrument
