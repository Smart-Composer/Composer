#pragma once

#include "RenderBridge.h"
#include <composer/instrument/InstrumentProcessor.h>

#include <atomic>

namespace composer::app
{
/** Adapts the shared processor to a Tracktion instrument position.

    The editing-thread owner applies the project patch and resets before disabling
    the plugin: Tracktion skips disabled built-ins, including incoming note-offs.
    Lifecycle changes and final destruction require a quiescent graph.
*/
class InstrumentAdapter final : public tracktion::Plugin
{
public:
    static constexpr const char* xmlTypeName = "composer_instrument_v1";
    static const char* getPluginName() { return "Composer Instrument"; }
    explicit InstrumentAdapter(tracktion::PluginCreationInfo);
    ~InstrumentAdapter() override;

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getVendor() override { return "Composer"; }
    juce::String getSelectableDescription() override { return getName(); }
    BusLayout getBusses() const override;
    bool isSynth() override { return true; }
    bool takesMidiInput() override { return true; }
    bool takesAudioInput() override { return false; }
    bool producesAudioWhenNoAudioInput() override { return true; }
    bool noTail() override { return false; }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getTailLength() const override { return processor.getTailLengthSeconds(); }

    void initialise(const tracktion::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void reset() override;
    void midiPanic() override;
    void applyToBuffer(const tracktion::PluginRenderContext&) override;

    // Explicit message-thread entry points; session validation/undo/revisions live outside.
    std::optional<contracts::ContractError> applyPatch(const contracts::InstrumentPatch&);
    contracts::InstrumentPatch currentPatch() const;
    unsigned consumeRenderFaults() noexcept { return renderFaults.exchange(0, std::memory_order_relaxed); }

private:
    instrument::InstrumentProcessor processor;
    RenderBridge bridge;
    bool prepared = false;
    std::atomic<unsigned> renderFaults {0};
    static_assert(std::atomic<unsigned>::is_always_lock_free);
};
}
