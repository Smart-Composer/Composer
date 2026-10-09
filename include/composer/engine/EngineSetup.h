#pragma once

#include <juce_core/juce_core.h>

#include <memory>

namespace tracktion
{
inline namespace engine
{
class Engine;
}
} // namespace tracktion

namespace composer::engine
{

/** Creates the sequencing engine for the interactive application.

    Devices open with the engine's defaults, except that no audio input channel opens until a
    feature asks for one. Settings persist in the roaming application-data folder and caches in
    the local one.
*/
std::unique_ptr<tracktion::Engine> createEngine(const juce::String& applicationName);

/** Creates a sequencing engine that opens no audio or MIDI device.

    It keeps no settings, and its cache and temporary files live under scratchDirectory,
    which the caller owns and removes. Offline rendering and automated checks use it.
*/
std::unique_ptr<tracktion::Engine> createHeadlessEngine(const juce::String& applicationName,
                                                        const juce::File& scratchDirectory);

} // namespace composer::engine
