#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace composer::app
{
enum class BridgeFault : unsigned { none = 0, notPrepared = 1, invalidContext = 2, invalidMidi = 4, capacityExceeded = 8 };

class RenderBridge
{
public:
    static constexpr int sliceCapacity = 1024;
    static constexpr std::size_t eventCapacity = 4096;
    static constexpr std::size_t midiEventStorageBytes = sizeof(juce::int32) + sizeof(juce::uint16) + 3;
    static_assert(midiEventStorageBytes == 9);

    RenderBridge();
    void prepare(double sampleRate);
    void release() noexcept;
    BridgeFault begin(const tracktion::PluginRenderContext&) noexcept;
    void prepareSlice(int offset, int count) noexcept;
    void copySliceTo(const tracktion::PluginRenderContext&, int offset, int count) noexcept;
    static void silenceDestination(const tracktion::PluginRenderContext&) noexcept;

    juce::AudioBuffer<float>& audio() noexcept { return audioView; }
    juce::MidiBuffer& midi() noexcept { return midiBuffer; }

private:
    struct Event
    {
        int sample = 0;
        int size = 0;
        std::array<std::uint8_t, 3> bytes {};
        int order = 0;
    };
    std::array<std::array<float, sliceCapacity>, 2> samples {};
    std::array<float*, 2> channels {samples[0].data(), samples[1].data()};
    juce::AudioBuffer<float> audioView;
    juce::MidiBuffer midiBuffer;
    std::array<Event, eventCapacity> events {};
    std::size_t eventCount = 0;
    double rate = 0.0;
};
}
