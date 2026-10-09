#include "RenderHarness.h"

#include <bit>
#include <cmath>
#include <limits>

namespace composer::tests
{

ScriptEvent noteOn(int sampleTime, int note, int velocity, int channel)
{
    return { sampleTime, { static_cast<std::uint8_t>(0x90 | (channel - 1)), static_cast<std::uint8_t>(note),
                           static_cast<std::uint8_t>(velocity) } };
}

ScriptEvent noteOff(int sampleTime, int note, int channel)
{
    return { sampleTime, { static_cast<std::uint8_t>(0x80 | (channel - 1)), static_cast<std::uint8_t>(note), 64 } };
}

ScriptEvent controller(int sampleTime, int number, int value, int channel)
{
    return { sampleTime, { static_cast<std::uint8_t>(0xB0 | (channel - 1)), static_cast<std::uint8_t>(number),
                           static_cast<std::uint8_t>(value) } };
}

MidiScript standardScript(double sampleRate)
{
    const auto at = [sampleRate](double seconds) { return static_cast<int>(std::lround(seconds * sampleRate)); };

    MidiScript script {
        noteOn(at(0.000), 60, 100),
        noteOn(at(0.050), 64, 64, 2),
        noteOn(at(0.100), 67, 127),
        noteOn(at(0.150), 48, 1),
        noteOff(at(0.400), 60),
        noteOn(at(0.450), 60, 90),
        noteOff(at(0.500), 64, 2),
        noteOff(at(0.600), 48),
        controller(at(0.650) + 3, 123, 0),
        noteOn(at(0.700), 72, 110, 2),
    };

    for (int index = 0; index < 18; ++index)
        script.push_back(noteOn(at(0.900) + 7 * index, 36 + 3 * index, 40 + 4 * index));

    for (int index = 0; index < 18; ++index)
        script.push_back(noteOff(at(1.300) + 5 * index, 36 + 3 * index));

    script.push_back(controller(at(1.500) + 11, 120, 0, 1));
    script.push_back(controller(at(1.500) + 11, 120, 0, 2));
    script.push_back(noteOn(at(1.700), 57, 100));
    script.push_back(noteOn(at(1.750), 81, 127, 2));
    script.push_back(noteOff(at(2.100), 57));
    script.push_back(noteOff(at(2.150), 81, 2));
    return script;
}

juce::AudioBuffer<float> renderScript(juce::AudioProcessor& processor,
                                      const MidiScript& script,
                                      int totalSamples,
                                      int blockSize,
                                      const std::function<void(int)>& beforeBlock)
{
    juce::AudioBuffer<float> output(2, totalSamples);
    juce::AudioBuffer<float> block(2, blockSize);
    juce::MidiBuffer midi;
    std::size_t nextEvent = 0;
    int blockIndex = 0;

    for (int start = 0; start < totalSamples; start += blockSize, ++blockIndex)
    {
        const int length = std::min(blockSize, totalSamples - start);
        block.setSize(2, length, false, false, true);

        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::fill(block.getWritePointer(channel),
                                              std::numeric_limits<float>::quiet_NaN(), length);

        midi.clear();

        while (nextEvent < script.size() && script[nextEvent].sampleTime < start + length)
        {
            const auto& event = script[nextEvent++];
            midi.addEvent(event.bytes.data(), 3, event.sampleTime - start);
        }

        if (beforeBlock)
            beforeBlock(blockIndex);

        processor.processBlock(block, midi);

        for (int channel = 0; channel < 2; ++channel)
            output.copyFrom(channel, start, block, channel, 0, length);
    }

    return output;
}

std::optional<int> firstBitDifference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return 0;

    for (int channel = 0; channel < a.getNumChannels(); ++channel)
        for (int index = 0; index < a.getNumSamples(); ++index)
            if (std::bit_cast<std::uint32_t>(a.getSample(channel, index))
                != std::bit_cast<std::uint32_t>(b.getSample(channel, index)))
                return channel * a.getNumSamples() + index;

    return std::nullopt;
}

float peak(const juce::AudioBuffer<float>& buffer, int start, int end)
{
    const int last = end < 0 ? buffer.getNumSamples() : end;
    float result = 0.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int index = start; index < last; ++index)
            result = std::max(result, std::abs(buffer.getSample(channel, index)));

    return result;
}

bool allFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int index = 0; index < buffer.getNumSamples(); ++index)
            if (! std::isfinite(buffer.getSample(channel, index)))
                return false;

    return true;
}

bool silentFrom(const juce::AudioBuffer<float>& buffer, int start, int end)
{
    const int last = end < 0 ? buffer.getNumSamples() : end;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int index = start; index < last; ++index)
            if (buffer.getSample(channel, index) != 0.0f)
                return false;

    return true;
}

} // namespace composer::tests
