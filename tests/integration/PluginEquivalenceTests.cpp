#include "HostedPluginState.h"
#include "PatchFixtures.h"
#include "RenderHarness.h"
#include "Vst3ReservedParameterIds.h"

#include <composer/instrument/InstrumentProcessor.h>
#include <composer/instrument/synth/ParameterCurves.h>

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <set>
#include <variant>

namespace
{

using namespace composer;
using namespace composer::tests;
using composer::instrument::InstrumentProcessor;
namespace synth = composer::instrument::synth;

std::unique_ptr<juce::AudioPluginInstance> instantiateBuiltPlugin(double sampleRate, int blockSize)
{
    const auto bundle = juce::File(COMPOSER_TEST_VST3_MODULE).getParentDirectory().getParentDirectory().getParentDirectory();
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, bundle.getFullPathName());
    REQUIRE(descriptions.size() == 1);

    juce::String error;
    auto instance = format.createInstanceFromDescription(*descriptions[0], sampleRate, blockSize, error);
    INFO(error.toStdString());
    REQUIRE(instance != nullptr);
    return instance;
}

std::string encoded(const contracts::InstrumentPatch& patch)
{
    return std::get<std::string>(contracts::encodePatch(patch));
}

void loadHostedPatch(juce::AudioPluginInstance& hosted, const contracts::InstrumentPatch& patch)
{
    const auto state = makeHostedState(encoded(patch));
    hosted.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
}

std::string hostedComponentBytes(juce::AudioPluginInstance& hosted)
{
    juce::MemoryBlock state;
    hosted.getStateInformation(state);
    const auto bytes = componentBytes(state);
    REQUIRE(bytes.has_value());
    return *bytes;
}

void prepareBoth(juce::AudioProcessor& a, juce::AudioProcessor& b, double rate, int block)
{
    for (auto* processor : { &a, &b })
    {
        processor->releaseResources();
        processor->setRateAndBufferSizeDetails(rate, block);
        processor->prepareToPlay(rate, block);
    }
}

juce::AudioProcessorParameter* hostedParameter(juce::AudioPluginInstance& hosted, std::uint32_t vst3Id)
{
    for (auto* parameter : hosted.getParameters())
        if (auto* withId = dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter))
            if (withId->getParameterID() == juce::String(static_cast<juce::int64>(vst3Id)))
                return parameter;

    return nullptr;
}

std::uint32_t vst3IdOf(std::size_t index)
{
    const auto& id = contracts::parameterDescriptors[index].id;
    return juce::VST3ClientExtensions::convertJuceParameterId(juce::String(id.data(), id.size()), true);
}

std::size_t indexOf(std::string_view id)
{
    std::size_t index = 0;
    while (contracts::parameterDescriptors[index].id != id)
        ++index;
    return index;
}

std::vector<std::pair<std::string, contracts::InstrumentPatch>> equivalencePatches()
{
    auto patches = loadPatchFixtures();
    patches.emplace_back("awkward", awkwardPatch());
    return patches;
}

} // namespace

TEST_CASE("The hosted VST3 exposes every patch parameter under its persistent identity")
{
    auto hosted = instantiateBuiltPlugin(48000.0, 128);
    InstrumentProcessor local;
    std::set<std::uint32_t> ids;

    for (std::size_t index = 0; index < synth::parameterCount; ++index)
    {
        const auto vst3Id = vst3IdOf(index);
        INFO(contracts::parameterDescriptors[index].id << " as " << vst3Id);
        auto* parameter = hostedParameter(*hosted, vst3Id);
        REQUIRE(parameter != nullptr);

        const auto* reference = local.getParameters()[static_cast<int>(index)];
        CHECK(parameter->getName(100) == reference->getName(100));
        CHECK(parameter->getDefaultValue() == reference->getDefaultValue());
        CHECK(parameter->isDiscrete() == reference->isDiscrete());

        if (reference->isDiscrete())
            CHECK(parameter->getNumSteps() == 3);

        CHECK(ids.insert(vst3Id).second);
        CHECK_FALSE(vst3::isReservedParameterId(vst3Id));
    }

    CHECK(hosted->getBypassParameter() != nullptr);
}

TEST_CASE("The VST3 wrapper's own parameters use the IDs the patch parameters avoid")
{
    auto hosted = instantiateBuiltPlugin(48000.0, 128);
    std::set<std::uint32_t> patchIds;

    for (std::size_t index = 0; index < synth::parameterCount; ++index)
        patchIds.insert(vst3IdOf(index));

    // Every parameter is a patch parameter, the bypass, or a MIDI controller in the reserved range.
    int bypassParameters = 0;
    std::uint32_t controllerParameters = 0;

    for (auto* parameter : hosted->getParameters())
    {
        const auto* withId = dynamic_cast<const juce::HostedAudioProcessorParameter*>(parameter);
        REQUIRE(withId != nullptr);
        const auto id = static_cast<std::uint32_t>(withId->getParameterID().getLargeIntValue());
        INFO(withId->getParameterID() << " named " << parameter->getName(100));

        if (patchIds.contains(id))
            continue;

        if (id == vst3::bypassParameterId)
        {
            ++bypassParameters;
            CHECK(parameter == hosted->getBypassParameter());
            continue;
        }

        CHECK(vst3::isReservedParameterId(id));
        CHECK(id != vst3::presetParameterId);
        ++controllerParameters;
    }

    CHECK(bypassParameters == 1);
    CHECK(controllerParameters == vst3::midiControllerParameterCount);

    // Controller parameters are numbered by channel, then controller, from the first channel's
    // bank select to the last channel's pitch bend.
    for (const auto& [channelIndex, controller] : { std::pair { 0u, 0u }, std::pair { 0u, 120u }, std::pair { 0u, 123u },
                                                    std::pair { 15u, 123u }, std::pair { 15u, 129u } })
    {
        INFO("channel index " << channelIndex << " controller " << controller);
        const auto* parameter = hostedParameter(*hosted, vst3::midiControllerParameterId(channelIndex, controller));
        REQUIRE(parameter != nullptr);
        CHECK(parameter->getName(100) == "MIDI CC " + juce::String(channelIndex) + "|" + juce::String(controller));
    }

    // With a single program the wrapper adds no program parameter.
    CHECK(hostedParameter(*hosted, vst3::presetParameterId) == nullptr);
}

TEST_CASE("A host restores the instrument's state exactly")
{
    for (const auto& [name, patch] : equivalencePatches())
    {
        INFO(name);
        auto hosted = instantiateBuiltPlugin(48000.0, 128);
        loadHostedPatch(*hosted, patch);

        const auto expected = encoded(patch);
        const auto component = hostedComponentBytes(*hosted);
        REQUIRE(component.size() >= expected.size() + sizeof(std::int64_t));
        CHECK(component.compare(0, expected.size(), expected) == 0);

        std::int64_t marker = -1;
        std::memcpy(&marker, component.data() + expected.size(), sizeof(marker));
        CHECK(marker == 0);

        const auto garbage = makeHostedState("not a patch at all");
        hosted->setStateInformation(garbage.getData(), static_cast<int>(garbage.getSize()));
        CHECK(hostedComponentBytes(*hosted).compare(0, expected.size(), expected) == 0);
    }
}

TEST_CASE("The in-process processor and the VST3 render the same patch bit for bit")
{
    for (const auto& [name, patch] : equivalencePatches())
    {
        for (const auto& [rate, block] : { std::pair { 48000.0, 128 }, std::pair { 44100.0, 512 } })
        {
            INFO(name << " at " << rate << " Hz in blocks of " << block);
            InstrumentProcessor local;
            REQUIRE_FALSE(local.applyPatch(patch).has_value());
            auto hosted = instantiateBuiltPlugin(rate, block);
            loadHostedPatch(*hosted, patch);
            prepareBoth(local, *hosted, rate, block);

            const auto script = standardScript(rate);
            const int total = static_cast<int>(2.5 * rate);
            const auto a = renderScript(local, script, total, block);
            const auto b = renderScript(*hosted, script, total, block);

            const auto difference = firstBitDifference(a, b);

            {
                INFO("first difference at flattened sample " << difference.value_or(0));
                CHECK_FALSE(difference.has_value());
            }

            CHECK(allFinite(a));

            if (name != "lower_bounds")
                CHECK(peak(a) > 0.01f);

            // All-sound-off on both channels at 1.5 s fades everything out within 3 ms, and
            // silence lasts until the next note at 1.7 s.
            const int fade = static_cast<int>(std::lround(0.003 * rate));
            CHECK(silentFrom(a, static_cast<int>(std::lround(1.5 * rate)) + 11 + fade, static_cast<int>(std::lround(1.7 * rate))));
        }
    }
}

TEST_CASE("Host automation reaches the in-process processor and the VST3 identically")
{
    const auto patch = awkwardPatch();
    InstrumentProcessor local;
    REQUIRE_FALSE(local.applyPatch(patch).has_value());
    auto hosted = instantiateBuiltPlugin(48000.0, 128);
    loadHostedPatch(*hosted, patch);
    prepareBoth(local, *hosted, 48000.0, 128);

    const auto cutoff = indexOf("cutoff_hz");
    const auto waveform = indexOf("waveform");
    auto* hostedCutoff = hostedParameter(*hosted, vst3IdOf(cutoff));
    auto* hostedWaveform = hostedParameter(*hosted, vst3IdOf(waveform));
    REQUIRE(hostedCutoff != nullptr);
    REQUIRE(hostedWaveform != nullptr);

    const auto script = standardScript(48000.0);
    const int total = 120000;

    const auto a = renderScript(local, script, total, 128, [&](int index) {
        if (index == 100)
        {
            local.getParameters()[static_cast<int>(cutoff)]->setValueNotifyingHost(0.25f);
            local.getParameters()[static_cast<int>(waveform)]->setValueNotifyingHost(0.5f);
        }
    });

    const auto b = renderScript(*hosted, script, total, 128, [&](int index) {
        if (index == 100)
        {
            hostedCutoff->setValue(0.25f);
            hostedWaveform->setValue(0.5f);
        }
    });

    CHECK_FALSE(firstBitDifference(a, b).has_value());

    const auto restored = contracts::decodePatch(hostedComponentBytes(*hosted).substr(0, encoded(local.currentPatch()).size()));
    REQUIRE(std::holds_alternative<contracts::InstrumentPatch>(restored));
    CHECK(std::get<contracts::InstrumentPatch>(restored).cutoffHz == synth::denormalise(cutoff, 0.25f));
    CHECK(std::get<contracts::InstrumentPatch>(restored) == local.currentPatch());
}

TEST_CASE("A saved host session restores the same instrument")
{
    juce::MemoryBlock session;
    std::string componentBefore;
    std::vector<float> valuesBefore;
    juce::AudioBuffer<float> renderBefore;
    const MidiScript script { noteOn(0, 60, 100), noteOn(300, 64, 80), noteOff(20000, 60) };

    {
        auto first = instantiateBuiltPlugin(48000.0, 128);
        loadHostedPatch(*first, awkwardPatch());
        hostedParameter(*first, vst3IdOf(indexOf("gain_db")))->setValue(0.4f);
        first->getStateInformation(session);
        componentBefore = hostedComponentBytes(*first);

        for (std::size_t index = 0; index < synth::parameterCount; ++index)
            valuesBefore.push_back(hostedParameter(*first, vst3IdOf(index))->getValue());

        first->setRateAndBufferSizeDetails(48000.0, 128);
        first->prepareToPlay(48000.0, 128);
        renderBefore = renderScript(*first, script, 48000, 128);
    }

    auto second = instantiateBuiltPlugin(48000.0, 128);
    second->setStateInformation(session.getData(), static_cast<int>(session.getSize()));
    CHECK(hostedComponentBytes(*second) == componentBefore);

    for (std::size_t index = 0; index < synth::parameterCount; ++index)
        CHECK(std::bit_cast<std::uint32_t>(hostedParameter(*second, vst3IdOf(index))->getValue())
              == std::bit_cast<std::uint32_t>(valuesBefore[index]));

    second->setRateAndBufferSizeDetails(48000.0, 128);
    second->prepareToPlay(48000.0, 128);
    CHECK_FALSE(firstBitDifference(renderScript(*second, script, 48000, 128), renderBefore).has_value());
}

TEST_CASE("The stop controllers and a host reset reach the VST3")
{
    auto patch = awkwardPatch();
    patch.releaseSeconds = 8.0;

    auto hosted = instantiateBuiltPlugin(48000.0, 256);
    loadHostedPatch(*hosted, patch);
    hosted->setRateAndBufferSizeDetails(48000.0, 256);
    hosted->prepareToPlay(48000.0, 256);

    constexpr int stopAt = 256 * 4 + 50;
    const MidiScript chord { noteOn(0, 60, 120), noteOn(0, 67, 120) };

    // All-sound-off fades the notes out within 3 ms.
    auto withAllSoundOff = chord;
    withAllSoundOff.push_back(controller(stopAt, 120, 0));
    const auto stopped = renderScript(*hosted, withAllSoundOff, 256 * 8, 256);
    CHECK(peak(stopped, 0, stopAt) > 0.01f);
    CHECK(silentFrom(stopped, stopAt + 144));

    // All-notes-off releases them, exactly as note-offs at the same sample would. A reset
    // between renders clears the long release tails.
    auto withNoteOffs = chord;
    withNoteOffs.push_back(noteOff(stopAt, 60));
    withNoteOffs.push_back(noteOff(stopAt, 67));
    hosted->reset();
    const auto released = renderScript(*hosted, withNoteOffs, 256 * 8, 256);

    auto withAllNotesOff = chord;
    withAllNotesOff.push_back(controller(stopAt, 123, 0));
    hosted->reset();
    const auto releasing = renderScript(*hosted, withAllNotesOff, 256 * 8, 256);
    CHECK(peak(releasing, stopAt + 144) > 0.01f);
    CHECK_FALSE(firstBitDifference(releasing, released).has_value());

    hosted->reset();
    const auto reset = renderScript(*hosted, { noteOn(0, 60, 120) }, 256 * 8, 256, [&](int index) {
        if (index == 4)
            hosted->reset();
    });
    CHECK(peak(reset, 0, 256 * 4) > 0.01f);
    CHECK(silentFrom(reset, 256 * 4));
}

TEST_CASE("The VST3 prepared again at a new sample rate renders like a fresh instance")
{
    const auto patch = awkwardPatch();
    auto hosted = instantiateBuiltPlugin(48000.0, 128);
    loadHostedPatch(*hosted, patch);
    hosted->setRateAndBufferSizeDetails(48000.0, 128);
    hosted->prepareToPlay(48000.0, 128);
    renderScript(*hosted, standardScript(48000.0), 48000, 128);

    hosted->releaseResources();
    hosted->setRateAndBufferSizeDetails(96000.0, 256);
    hosted->prepareToPlay(96000.0, 256);

    auto fresh = instantiateBuiltPlugin(96000.0, 256);
    loadHostedPatch(*fresh, patch);
    InstrumentProcessor local;
    REQUIRE_FALSE(local.applyPatch(patch).has_value());
    prepareBoth(*fresh, local, 96000.0, 256);

    const auto script = standardScript(96000.0);
    const int total = static_cast<int>(2.5 * 96000.0);
    const auto again = renderScript(*hosted, script, total, 256);

    CHECK(peak(again) > 0.01f);
    CHECK_FALSE(firstBitDifference(again, renderScript(*fresh, script, total, 256)).has_value());
    CHECK_FALSE(firstBitDifference(again, renderScript(local, script, total, 256)).has_value());
}

TEST_CASE("A note released in the VST3 ends in its release time")
{
    auto patch = awkwardPatch();
    patch.releaseSeconds = 0.25;
    patch.sustainLevel = 1.0;

    auto hosted = instantiateBuiltPlugin(48000.0, 128);
    loadHostedPatch(*hosted, patch);
    hosted->setRateAndBufferSizeDetails(48000.0, 128);
    hosted->prepareToPlay(48000.0, 128);

    constexpr int noteOffAt = 12000;
    const auto output = renderScript(*hosted, { noteOn(0, 69, 127), noteOff(noteOffAt, 69) }, 48000, 128);
    CHECK(peak(output, noteOffAt, noteOffAt + 2400) > 0.0f);
    CHECK(silentFrom(output, noteOffAt + static_cast<int>(std::ceil(0.25 * 48000.0)) + 2));
}

TEST_CASE("Bypassing the VST3 fades it out and leaves no note hanging")
{
    auto hosted = instantiateBuiltPlugin(48000.0, 256);
    loadHostedPatch(*hosted, awkwardPatch());
    hosted->setRateAndBufferSizeDetails(48000.0, 256);
    hosted->prepareToPlay(48000.0, 256);

    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    const std::array<std::uint8_t, 3> on { 0x90, 60, 110 };
    midi.addEvent(on.data(), 3, 0);
    hosted->processBlock(buffer, midi);
    CHECK(peak(buffer) > 0.01f);
    const float last = buffer.getSample(0, 255);

    // A note-on while bypassed is ignored, and the held note fades out over 3 ms.
    const std::array<std::uint8_t, 3> another { 0x90, 64, 110 };
    midi.clear();
    midi.addEvent(another.data(), 3, 10);
    hosted->processBlockBypassed(buffer, midi);
    CHECK(std::abs(buffer.getSample(0, 0) - last) < 0.05f);
    CHECK(peak(buffer, 0, 10) > 0.0f);
    CHECK(silentFrom(buffer, 144));

    midi.clear();
    hosted->processBlock(buffer, midi);
    CHECK(silentFrom(buffer, 0));

    midi.addEvent(on.data(), 3, 0);
    hosted->processBlock(buffer, midi);
    CHECK(peak(buffer) > 0.01f);
}
