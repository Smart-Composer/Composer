#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>
#include <string>

namespace composer::tests
{

/** A JUCE host's saved state for a VST3 plugin whose component state is exactly componentBytes.

    This is the layout JUCE's VST3 host reads and writes: an XML element named VST3PluginState
    whose IComponent child holds the component state in JUCE's MemoryBlock base-64 form, stored
    as binary XML.
*/
juce::MemoryBlock makeHostedState(const std::string& componentBytes);

/** The component state inside a JUCE host's saved VST3 state. */
std::optional<std::string> componentBytes(const juce::MemoryBlock& hostedState);

} // namespace composer::tests
