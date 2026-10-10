#include "InstrumentAdapter.h"

#include <algorithm>
#include <cmath>

namespace composer::app
{
InstrumentAdapter::InstrumentAdapter(tracktion::PluginCreationInfo info) : Plugin(info) {}

InstrumentAdapter::~InstrumentAdapter()
{
    notifyListenersOfDeletion();
    // The graph must already be detached before releasing the final plugin reference.
    if (prepared)
        deinitialise();
}

tracktion::Plugin::BusLayout InstrumentAdapter::getBusses() const
{
    BusLayout layout;
    layout.outputs.push_back(tracktion::ChannelConfiguration::stereo());
    return layout;
}

void InstrumentAdapter::initialise(const tracktion::PluginInitialisationInfo& info)
{
    if (prepared)
        deinitialise();
    bridge.prepare(info.sampleRate);
    if (!std::isfinite(info.sampleRate) || info.sampleRate <= 0.0)
    {
        renderFaults.fetch_or(static_cast<unsigned>(BridgeFault::invalidContext), std::memory_order_relaxed);
        return;
    }
    processor.setRateAndBufferSizeDetails(info.sampleRate, RenderBridge::sliceCapacity);
    processor.prepareToPlay(info.sampleRate, RenderBridge::sliceCapacity);
    prepared = true;
}

void InstrumentAdapter::deinitialise()
{
    processor.panic();
    if (prepared)
        processor.releaseResources();
    prepared = false;
    bridge.release();
}

void InstrumentAdapter::reset() { processor.reset(); }
void InstrumentAdapter::midiPanic() { processor.panic(); }

std::optional<contracts::ContractError> InstrumentAdapter::applyPatch(const contracts::InstrumentPatch& patch)
{
    return processor.applyPatch(patch);
}

contracts::InstrumentPatch InstrumentAdapter::currentPatch() const { return processor.currentPatch(); }

void InstrumentAdapter::applyToBuffer(const tracktion::PluginRenderContext& context)
{
    if (context.bufferForMidiMessages != nullptr && context.bufferForMidiMessages->isAllNotesOff)
        processor.panic();
    const auto fault = bridge.begin(context);
    if (fault != BridgeFault::none)
    {
        processor.reset();
        renderFaults.fetch_or(static_cast<unsigned>(fault), std::memory_order_relaxed);
        RenderBridge::silenceDestination(context);
        return;
    }
    // Preserve the incoming MIDI array as Tracktion's built-in synth does. No MIDI
    // ownership transfers, allocations or outgoing events are made by this adapter.
    for (int offset = 0; offset < context.bufferNumSamples;)
    {
        const auto count = std::min(RenderBridge::sliceCapacity, context.bufferNumSamples - offset);
        bridge.prepareSlice(offset, count);
        processor.processBlock(bridge.audio(), bridge.midi());
        bridge.copySliceTo(context, offset, count);
        offset += count;
    }
}
}
