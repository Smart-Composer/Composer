#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace composer::tests
{

/** A three-byte MIDI message at an absolute sample time. */
struct ScriptEvent
{
    int sampleTime = 0;
    std::array<std::uint8_t, 3> bytes {};
};

using MidiScript = std::vector<ScriptEvent>;

ScriptEvent noteOn(int sampleTime, int note, int velocity, int channel = 1);
ScriptEvent noteOff(int sampleTime, int note, int channel = 1);
ScriptEvent controller(int sampleTime, int number, int value, int channel = 1);

/** A two-and-a-half-second performance: several velocities and two channels, overlapping notes,
    a retrigger, a chord large enough to steal voices, releases, all-notes-off on the first
    channel and all-sound-off on both at 1.5 s, at samples no note uses. */
MidiScript standardScript(double sampleRate);

/** Renders a script through a processor in fixed-size blocks, stereo. Each block's buffer
    starts filled with NaN, so a sample the processor leaves unwritten stays NaN. beforeBlock runs
    before each block with the block's index. */
juce::AudioBuffer<float> renderScript(juce::AudioProcessor& processor,
                                      const MidiScript& script,
                                      int totalSamples,
                                      int blockSize,
                                      const std::function<void(int)>& beforeBlock = {});

/** The first sample, as channel * length + index, whose float bits differ. */
std::optional<int> firstBitDifference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b);

float peak(const juce::AudioBuffer<float>& buffer, int start = 0, int end = -1);
bool allFinite(const juce::AudioBuffer<float>& buffer);
bool silentFrom(const juce::AudioBuffer<float>& buffer, int start, int end = -1);

} // namespace composer::tests
