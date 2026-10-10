#include "RenderBridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace composer::app
{
RenderBridge::RenderBridge() : audioView(channels.data(), 2, sliceCapacity) {}

void RenderBridge::prepare(double sampleRate)
{
    // Pinned JUCE stores each event as int32 sample + uint16 length + payload.
    // Channel messages need at most 9 bytes; this bound follows that exact layout.
    midiBuffer.ensureSize(eventCapacity * midiEventStorageBytes);
    rate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 0.0;
    eventCount = 0;
    midiBuffer.clear();
}

void RenderBridge::release() noexcept
{
    rate = 0.0;
    eventCount = 0;
    midiBuffer.clear();
}

BridgeFault RenderBridge::begin(const tracktion::PluginRenderContext& context) noexcept
{
    eventCount = 0;
    if (rate == 0.0)
        return BridgeFault::notPrepared;
    if (context.bufferStartSample < 0 || context.bufferNumSamples < 0 || !std::isfinite(context.midiBufferOffset))
        return BridgeFault::invalidContext;
    if (context.destBuffer != nullptr
        && (context.bufferStartSample > context.destBuffer->getNumSamples()
            || context.bufferNumSamples > context.destBuffer->getNumSamples() - context.bufferStartSample))
        return BridgeFault::invalidContext;
    if (context.bufferForMidiMessages == nullptr || context.bufferNumSamples == 0)
        return BridgeFault::none;
    // Bound scan work before touching any events. Long/system messages are unsupported,
    // but still count against this input-complexity limit.
    if (context.bufferForMidiMessages->size() > static_cast<int>(eventCapacity))
        return BridgeFault::capacityExceeded;

    const auto duration = static_cast<double>(context.bufferNumSamples) / rate;
    for (const auto& message : *context.bufferForMidiMessages)
    {
        const auto time = message.getTimeStamp() + context.midiBufferOffset;
        if (!std::isfinite(time))
            return BridgeFault::invalidMidi;
        // Late events run at sample zero (in particular, don't strand a late note-off).
        // Events at/after the end belong to a later context; input remains untouched.
        if (time >= duration)
            continue;
        const auto* bytes = message.getRawData();
        const auto size = message.getRawDataSize();
        if (size == 0 || bytes[0] < 0x80 || bytes[0] > 0xef)
            continue;
        const int kind = bytes[0] & 0xf0;
        const int expectedSize = kind == 0xc0 || kind == 0xd0 ? 2 : 3;
        if (size != expectedSize || bytes[1] > 0x7f || (size == 3 && bytes[2] > 0x7f))
            return BridgeFault::invalidMidi;
        auto& event = events[eventCount];
        event.order = static_cast<int>(eventCount++);
        // Nearest sample, with the final fractional sample kept inside the context.
        const auto nearest = std::floor(std::max(0.0, time) * rate + 0.5);
        event.sample = static_cast<int>(std::min(nearest, static_cast<double>(context.bufferNumSamples - 1)));
        event.size = size;
        std::memcpy(event.bytes.data(), bytes, static_cast<std::size_t>(size));
    }
    // The tie breaker preserves source order when rounding or late-event clamping
    // puts several messages at one sample. std::stable_sort could allocate here.
    std::sort(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(eventCount),
              [](const Event& left, const Event& right) noexcept {
                  return left.sample < right.sample
                      || (left.sample == right.sample && left.order < right.order);
              });
    return BridgeFault::none;
}

void RenderBridge::prepareSlice(int offset, int count) noexcept
{
    // Caller supplies nonnegative subranges of at most sliceCapacity samples.
    audioView.setDataToReferTo(channels.data(), 2, count);
    audioView.clear();
    midiBuffer.clear();
    for (std::size_t index = 0; index < eventCount; ++index)
    {
        const auto& event = events[index];
        if (event.sample >= offset && event.sample - offset < count)
        {
            // At the pinned JUCE revision MidiBuffer::addEvent searches from the
            // start on every insertion. Append already-sorted packed records to
            // its public byte array instead; preparation reserved the full bound.
            std::array<juce::uint8, midiEventStorageBytes> record {};
            juce::writeUnaligned<juce::int32>(record.data(), event.sample - offset);
            juce::writeUnaligned<juce::uint16>(record.data() + sizeof(juce::int32),
                                              static_cast<juce::uint16>(event.size));
            constexpr auto headerBytes = sizeof(juce::int32) + sizeof(juce::uint16);
            std::memcpy(record.data() + headerBytes, event.bytes.data(), static_cast<std::size_t>(event.size));
            midiBuffer.data.addArray(record.data(), static_cast<int>(headerBytes) + event.size);
        }
    }
}

void RenderBridge::copySliceTo(const tracktion::PluginRenderContext& context, int offset, int count) noexcept
{
    if (context.destBuffer == nullptr)
        return;
    const auto channelCount = context.destBuffer->getNumChannels();
    const auto destinationStart = context.bufferStartSample + offset;
    if (channelCount == 1)
    {
        context.destBuffer->copyFrom(0, destinationStart, audioView, 0, 0, count);
        context.destBuffer->addFrom(0, destinationStart, audioView, 1, 0, count);
        context.destBuffer->applyGain(0, destinationStart, count, 0.5f);
    }
    else
    {
        for (int channel = 0; channel < std::min(2, channelCount); ++channel)
            context.destBuffer->copyFrom(channel, destinationStart, audioView, channel, 0, count);
    }
    for (int channel = 2; channel < channelCount; ++channel)
        context.destBuffer->clear(channel, destinationStart, count);
}

void RenderBridge::silenceDestination(const tracktion::PluginRenderContext& context) noexcept
{
    if (context.destBuffer == nullptr || context.bufferStartSample < 0 || context.bufferNumSamples < 0
        || context.bufferStartSample > context.destBuffer->getNumSamples()
        || context.bufferNumSamples > context.destBuffer->getNumSamples() - context.bufferStartSample)
        return;
    context.destBuffer->clear(context.bufferStartSample, context.bufferNumSamples);
}
}
