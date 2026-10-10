#pragma once

#include <cstdint>

namespace composer::tests::vst3
{

// The VST3 parameter IDs that the pinned JUCE 9.0.3 VST3 wrapper gives to parameters of its own,
// from modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp:
//
// - lines 407-412, JuceAudioProcessor::InternalParameters: paramPreset 0x70727374 ('prst'),
//   paramMidiControllerOffset 0x6d636d00 ('mdm*') and paramBypass 0x62797073 ('byps').
// - line 705: the bypass parameter the wrapper provides takes paramBypass when the processor uses
//   managed parameters, as the instrument does.
// - lines 717-731 and 751: the program parameter, programParamID = paramPreset, exists only with
//   more than one program; the instrument has one.
// - lines 1616-1619 and 1769-1795, with JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS: one parameter
//   per MIDI channel (numMIDIChannels = 16) and controller number (Vst::kCountCtrlNumber = 130,
//   controllers 0-127, aftertouch and pitch bend; pluginterfaces/vst/ivstmidicontrollers.h line
//   107 in the VST3 SDK that JUCE bundles), numbered as offset + channel * 130 + controller from
//   paramMidiControllerOffset when the processor uses managed parameters (line 1770), as the
//   instrument does.
//
// A patch parameter whose VST3 ID equals one of these, or falls in the controller range, would
// collide with a wrapper parameter. The integration tests confirm on the built VST3 the bypass ID
// and the controller numbering, and that it has no program parameter.

inline constexpr std::uint32_t presetParameterId = 0x70727374u;
inline constexpr std::uint32_t bypassParameterId = 0x62797073u;
inline constexpr std::uint32_t midiControllerParameterOffset = 0x6d636d00u;
inline constexpr std::uint32_t midiControllerChannels = 16;
inline constexpr std::uint32_t midiControllersPerChannel = 130;
inline constexpr std::uint32_t midiControllerParameterCount = midiControllerChannels * midiControllersPerChannel;

constexpr std::uint32_t midiControllerParameterId(std::uint32_t channelIndex, std::uint32_t controller)
{
    return midiControllerParameterOffset + channelIndex * midiControllersPerChannel + controller;
}

constexpr bool isReservedParameterId(std::uint32_t id)
{
    return id == presetParameterId
        || id == bypassParameterId
        || (id >= midiControllerParameterOffset && id - midiControllerParameterOffset < midiControllerParameterCount);
}

} // namespace composer::tests::vst3
