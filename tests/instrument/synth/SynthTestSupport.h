#pragma once

#include <composer/instrument/synth/Synth.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace composer::instrument::synth::testing
{

/** A patch for tests, with every field given. The defaults are a plain, open, unity-gain voice. */
inline contracts::InstrumentPatch makePatch(contracts::Waveform waveform = contracts::Waveform::sine,
                                            double gainDb = 0.0,
                                            double attackSeconds = 0.0,
                                            double decaySeconds = 0.0,
                                            double sustainLevel = 1.0,
                                            double releaseSeconds = 0.0,
                                            double cutoffHz = 20000.0,
                                            double resonanceQ = 0.7071067811865476)
{
    contracts::InstrumentPatch patch;
    patch.waveform = waveform;
    patch.gainDb = gainDb;
    patch.attackSeconds = attackSeconds;
    patch.decaySeconds = decaySeconds;
    patch.sustainLevel = sustainLevel;
    patch.releaseSeconds = releaseSeconds;
    patch.cutoffHz = cutoffHz;
    patch.resonanceQ = resonanceQ;
    return patch;
}

/** A raw three-byte MIDI message at a sample position. */
struct Event
{
    int sample = 0;
    std::array<std::uint8_t, 3> bytes {};
};

inline Event noteOnAt(int sample, int note, int velocity, int channel = 0)
{
    return { sample, { static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(note),
                       static_cast<std::uint8_t>(velocity) } };
}

inline Event noteOffAt(int sample, int note, int channel = 0)
{
    return { sample, { static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(note), 0 } };
}

inline Event controllerAt(int sample, int controller, int value, int channel = 0)
{
    return { sample, { static_cast<std::uint8_t>(0xB0 | channel), static_cast<std::uint8_t>(controller),
                       static_cast<std::uint8_t>(value) } };
}

/** Renders events in sample order, with the render split into the given block sizes, repeated
    as needed, or in one block when none are given. Events are applied at their exact sample, as
    the processor applies them. */
inline std::vector<float> renderEvents(Synth& synth, const std::vector<Event>& events, int totalSamples,
                                       std::vector<int> blockSizes = {})
{
    if (blockSizes.empty())
        blockSizes.push_back(std::max(1, totalSamples));

    std::vector<float> output(static_cast<std::size_t>(totalSamples), -1.0f);
    std::size_t nextEvent = 0;
    std::size_t nextBlock = 0;
    int position = 0;

    while (position < totalSamples)
    {
        const int blockSize = blockSizes[nextBlock++ % blockSizes.size()];

        if (blockSize <= 0)
        {
            synth.render(output.data() + position, 0);
            continue;
        }

        const int blockEnd = std::min(totalSamples, position + blockSize);

        while (nextEvent < events.size() && events[nextEvent].sample < blockEnd)
        {
            const int at = std::max(position, events[nextEvent].sample);
            synth.render(output.data() + position, at - position);
            position = at;
            synth.handleMidi(events[nextEvent].bytes.data(), 3);
            ++nextEvent;
        }

        synth.render(output.data() + position, blockEnd - position);
        position = blockEnd;
    }

    for (; nextEvent < events.size(); ++nextEvent)
        synth.handleMidi(events[nextEvent].bytes.data(), 3);

    return output;
}

/** The first index whose float bits differ, if any. */
inline std::optional<std::size_t> firstBitDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return std::min(a.size(), b.size());

    for (std::size_t index = 0; index < a.size(); ++index)
        if (std::bit_cast<std::uint32_t>(a[index]) != std::bit_cast<std::uint32_t>(b[index]))
            return index;

    return std::nullopt;
}

inline double peak(const std::vector<float>& samples, std::size_t from = 0, std::size_t to = SIZE_MAX)
{
    double result = 0.0;

    for (std::size_t index = from; index < std::min(to, samples.size()); ++index)
        result = std::max(result, static_cast<double>(std::abs(samples[index])));

    return result;
}

inline bool allFinite(const std::vector<float>& samples)
{
    for (const auto sample : samples)
        if (! std::isfinite(sample))
            return false;

    return true;
}

/** Largest absolute sample-to-sample step over a range. */
inline double largestStep(const std::vector<float>& samples, std::size_t from, std::size_t to)
{
    double result = 0.0;

    for (std::size_t index = std::max<std::size_t>(from, 1); index < std::min(to, samples.size()); ++index)
        result = std::max(result, std::abs(static_cast<double>(samples[index]) - samples[index - 1]));

    return result;
}

/** Largest absolute second difference over a range. A smooth signal's second difference is small;
    a discontinuity in value or slope shows up as a spike. */
inline double largestSecondDifference(const std::vector<float>& samples, std::size_t from, std::size_t to)
{
    double result = 0.0;

    for (std::size_t index = std::max<std::size_t>(from, 2); index < std::min(to, samples.size()); ++index)
        result = std::max(result, std::abs(static_cast<double>(samples[index]) - 2.0 * samples[index - 1]
                                           + samples[index - 2]));

    return result;
}

} // namespace composer::instrument::synth::testing
