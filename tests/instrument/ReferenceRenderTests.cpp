#include "ReferencePatches.h"
#include "RenderHarness.h"
#include "Sha256.h"

#include <composer/contracts/InstrumentPatch.h>
#include <composer/instrument/InstrumentProcessor.h>
#include <composer/instrument/synth/PatchState.h>
#include <composer/instrument/synth/Synth.h>
#include <composer_v1/contracts/InstrumentPatch.h>
#include <composer_v1/instrument/InstrumentProcessor.h>
#include <composer_v1/instrument/synth/PatchState.h>
#include <composer_v1/instrument/synth/Synth.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using namespace composer::tests;
using LiveProcessor = composer::instrument::InstrumentProcessor;
using ReferenceProcessor = composer_v1::instrument::InstrumentProcessor;
namespace liveSynth = composer::instrument::synth;
namespace v1Synth = composer_v1::instrument::synth;

// The v1 host parameters by persistent ID, in v1's order. Steps name parameters by their place in
// this list, and each instrument finds them by ID, so a step drives the same parameter on both
// whatever order a later version gives its parameters.
constexpr std::array<const char*, 8> v1ParameterIds { "waveform",      "gain_db",         "attack_seconds", "decay_seconds",
                                                      "sustain_level", "release_seconds", "cutoff_hz",      "resonance_q" };

/** Something a host or the embedding code does between blocks. */
enum class Action
{
    set,        // A host writes value to a parameter.
    echoAll,    // A host writes back every parameter's value as the frozen instrument reports it.
    panic,      // The embedding code fades every note out.
    bypass,     // The host starts processing bypassed,
    resume,     // and stops.
    reset,      // The host breaks the audio.
    apply,      // The embedding code applies the next saved patch.
    restore,    // The host restores the render's own saved patch.
    reprepare   // The host prepares again at another sample rate.
};

/** An action, taken before the block that contains its time. */
struct Step
{
    double seconds;
    Action action;
    int parameter = 0;
    float value = 0.0f;
};

constexpr Step hostWrite(double seconds, int parameter, float value)
{
    return { seconds, Action::set, parameter, value };
}

// Every host parameter, the waveform included, reaches both ends of its normalised range and
// values between them, several in one block, while notes start, sustain and release. The first
// step writes back the value each parameter already has, which keeps the patch's exact value.
const std::vector<Step> automationSteps {
    { 0.05, Action::echoAll },
    hostWrite(0.12, 6, 0.0f), hostWrite(0.12, 7, 1.0f), hostWrite(0.20, 0, 0.5f), hostWrite(0.26, 1, 1.0f),
    hostWrite(0.31, 6, 1.0f), hostWrite(0.33, 2, 0.0f), hostWrite(0.36, 3, 1.0f), hostWrite(0.42, 4, 0.0f),
    hostWrite(0.47, 5, 1.0f), hostWrite(0.55, 7, 0.0f), hostWrite(0.62, 0, 1.0f), hostWrite(0.68, 3, 0.0f),
    hostWrite(0.74, 4, 1.0f), hostWrite(0.80, 1, 0.0f), hostWrite(0.83, 2, 1.0f), hostWrite(0.86, 5, 0.0f),
    hostWrite(0.88, 0, 0.0f),
    hostWrite(0.89, 1, 0.75f), hostWrite(0.89, 2, 0.1f), hostWrite(0.89, 3, 0.4f), hostWrite(0.89, 4, 0.6f),
    hostWrite(0.89, 5, 0.35f), hostWrite(0.89, 6, 0.7f), hostWrite(0.89, 7, 0.45f),
    hostWrite(1.10, 6, 0.3333333f), hostWrite(1.15, 7, 0.9f), hostWrite(1.20, 0, 0.75f), hostWrite(1.25, 1, 0.5f),
    hostWrite(1.45, 0, 0.25f), hostWrite(1.60, 4, 0.0f), hostWrite(1.60, 5, 0.05f), hostWrite(1.72, 6, 0.0f),
    hostWrite(1.80, 6, 1.0f), hostWrite(1.85, 1, 1.0f), hostWrite(1.90, 2, 0.0f),
    { 1.95, Action::panic },
    hostWrite(2.00, 7, 1.0f), hostWrite(2.05, 3, 0.2f), hostWrite(2.20, 4, 0.5f), hostWrite(2.30, 5, 0.6f),
};

// Bypass with notes sounding and arriving, a patch applied and a state restored mid-note, each
// followed by the host writing back what it was told, a reset, a panic and a second prepare at
// another rate, after which the cutoff reaches its top.
const std::vector<Step> lifecycleSteps {
    { 0.05, Action::echoAll },
    hostWrite(0.20, 1, 0.5f),
    { 0.25, Action::bypass },
    { 0.31, Action::resume },
    { 0.36, Action::apply },
    { 0.38, Action::echoAll },
    hostWrite(0.45, 6, 0.8f),
    { 0.55, Action::reset },
    { 0.70, Action::restore },
    { 0.72, Action::echoAll },
    hostWrite(0.80, 0, 1.0f),
    { 0.95, Action::panic },
    { 1.10, Action::reprepare },
    hostWrite(1.60, 6, 1.0f),
    hostWrite(1.62, 7, 1.0f),
    { 2.00, Action::bypass },
    { 2.02, Action::resume },
};

int sampleAt(double seconds, double sampleRate)
{
    return static_cast<int>(std::lround(seconds * sampleRate));
}

void sortByTime(MidiScript& script)
{
    std::stable_sort(script.begin(), script.end(),
                     [](const ScriptEvent& a, const ScriptEvent& b) { return a.sampleTime < b.sampleTime; });
}

/** The standard performance with two-byte messages, which the instrument ignores, more notes and
    stop controllers inside blocks, and two notes above half of the lowest sample rate. */
MidiScript automationScript(double sampleRate)
{
    const auto at = [sampleRate](double seconds) { return sampleAt(seconds, sampleRate); };

    auto script = standardScript(sampleRate);
    script.insert(script.end(), {
        programChange(at(0.21), 5),
        channelPressure(at(0.33) + 2, 90),
        noteOn(at(1.00) + 3, 50, 120),
        noteOn(at(1.00) + 3, 62, 90, 2),
        channelPressure(at(0.95), 10, 2),
        controller(at(1.05) + 9, 120, 0, 2),
        noteOn(at(1.20) + 1, 74, 70, 2),
        controller(at(1.40) + 2, 123, 0, 1),
        programChange(at(1.73), 0, 2),
        noteOn(at(1.96), 65, 100),
        noteOn(at(2.00), 69, 110, 2),
        noteOn(at(2.20), 127, 100, 2),
        noteOn(at(2.21) + 1, 125, 80),
        controller(at(2.25) + 13, 123, 0, 2),
        noteOff(at(2.30), 65),
        controller(at(2.35) + 1, 120, 0, 1),
        noteOn(at(2.40) + 5, 45, 127, 2),
    });

    sortByTime(script);
    return script;
}

/** The automation performance with notes that arrive and end while bypassed, and high notes after
    the second prepare. */
MidiScript lifecycleScript(double sampleRate)
{
    const auto at = [sampleRate](double seconds) { return sampleAt(seconds, sampleRate); };

    auto script = automationScript(sampleRate);
    script.insert(script.end(), {
        noteOn(at(0.27), 70, 100),
        noteOn(at(0.28) + 5, 62, 80, 2),
        noteOff(at(0.29), 67),
        noteOn(at(1.80), 127, 100),
        noteOn(at(1.81), 124, 90, 2),
        noteOn(at(2.01), 52, 100),
    });

    sortByTime(script);
    return script;
}

enum class Performance
{
    standard,
    automation,
    lifecycle
};

const char* nameOf(Performance performance)
{
    switch (performance)
    {
        case Performance::standard: return "standard";
        case Performance::automation: return "automation";
        case Performance::lifecycle: return "lifecycle";
    }

    return "";
}

MidiScript scriptFor(Performance performance, double sampleRate)
{
    switch (performance)
    {
        case Performance::standard: return standardScript(sampleRate);
        case Performance::automation: return automationScript(sampleRate);
        case Performance::lifecycle: return lifecycleScript(sampleRate);
    }

    return {};
}

const std::vector<Step>& stepsFor(Performance performance)
{
    static const std::vector<Step> none;

    switch (performance)
    {
        case Performance::standard: return none;
        case Performance::automation: return automationSteps;
        case Performance::lifecycle: return lifecycleSteps;
    }

    return none;
}

// The three common host settings, and one rate low enough that the cutoff limit of 0.49 times the
// sample rate lies below the cutoff's top and that the highest notes lie above half the rate.
constexpr std::array<std::pair<double, int>, 4> settings {
    { { 44100.0, 512 }, { 48000.0, 128 }, { 96000.0, 256 }, { 22050.0, 64 } }
};

/** The parameters with the v1 IDs, in v1's order. */
std::array<juce::AudioProcessorParameter*, v1ParameterIds.size()> v1Parameters(juce::AudioProcessor& processor)
{
    std::array<juce::AudioProcessorParameter*, v1ParameterIds.size()> found {};

    for (auto* parameter : processor.getParameters())
        if (const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*>(parameter))
            for (std::size_t index = 0; index < v1ParameterIds.size(); ++index)
                if (withId->paramID == v1ParameterIds[index])
                    found[index] = parameter;

    for (std::size_t index = 0; index < found.size(); ++index)
    {
        INFO("parameter " << v1ParameterIds[index]);
        REQUIRE(found[index] != nullptr);
    }

    return found;
}

/** The live patch index of each v1 parameter. */
std::array<std::size_t, v1ParameterIds.size()> liveIndices()
{
    std::array<std::size_t, v1ParameterIds.size()> indices {};

    for (std::size_t index = 0; index < v1ParameterIds.size(); ++index)
    {
        const auto& descriptors = composer::contracts::parameterDescriptors;
        const auto found = std::find_if(descriptors.begin(), descriptors.end(),
                                        [&](const auto& descriptor) { return descriptor.id == v1ParameterIds[index]; });
        INFO("parameter " << v1ParameterIds[index]);
        REQUIRE(found != descriptors.end());
        indices[index] = static_cast<std::size_t>(found - descriptors.begin());
    }

    return indices;
}

composer::contracts::InstrumentPatch decodeLive(const std::string& text)
{
    const auto decoded = composer::contracts::decodePatch(text);
    REQUIRE(std::holds_alternative<composer::contracts::InstrumentPatch>(decoded));
    return std::get<composer::contracts::InstrumentPatch>(decoded);
}

composer_v1::contracts::InstrumentPatch decodeReference(const std::string& text)
{
    const auto decoded = composer_v1::contracts::decodePatch(text);
    REQUIRE(std::holds_alternative<composer_v1::contracts::InstrumentPatch>(decoded));
    return std::get<composer_v1::contracts::InstrumentPatch>(decoded);
}

void restore(juce::AudioProcessor& processor, const std::string& text)
{
    processor.setStateInformation(text.data(), static_cast<int>(text.size()));
}

void prepare(juce::AudioProcessor& processor, double sampleRate, int blockSize)
{
    processor.setRateAndBufferSizeDetails(sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);
}

struct Renders
{
    juce::AudioBuffer<float> live;
    juce::AudioBuffer<float> reference;
};

/** Renders a performance through both processors in the same fixed-size blocks. Each starts from
    the saved patch restored as host state; apply and restore steps use the next saved patch and
    the render's own. Every block's buffer starts filled with NaN. */
Renders renderBoth(const SavedV1Patch& saved, const SavedV1Patch& next, Performance performance, double sampleRate, int blockSize)
{
    const auto script = scriptFor(performance, sampleRate);
    const auto& steps = stepsFor(performance);
    const int total = static_cast<int>(2.5 * sampleRate);

    LiveProcessor live;
    ReferenceProcessor reference;
    restore(live, saved.text);
    restore(reference, saved.text);
    prepare(live, sampleRate, blockSize);
    prepare(reference, sampleRate, blockSize);

    const auto liveParameters = v1Parameters(live);
    const auto referenceParameters = v1Parameters(reference);

    Renders renders { juce::AudioBuffer<float>(2, total), juce::AudioBuffer<float>(2, total) };
    juce::AudioBuffer<float> block(2, blockSize);
    juce::MidiBuffer midi;
    std::size_t nextEvent = 0;
    std::size_t nextStep = 0;
    bool bypassed = false;

    for (int start = 0; start < total; start += blockSize)
    {
        const int length = std::min(blockSize, total - start);

        for (; nextStep < steps.size() && static_cast<int>(std::floor(steps[nextStep].seconds * sampleRate)) < start + length; ++nextStep)
        {
            const auto& step = steps[nextStep];

            switch (step.action)
            {
                case Action::set:
                    liveParameters[static_cast<std::size_t>(step.parameter)]->setValueNotifyingHost(step.value);
                    referenceParameters[static_cast<std::size_t>(step.parameter)]->setValueNotifyingHost(step.value);
                    break;

                case Action::echoAll:
                    for (std::size_t index = 0; index < v1ParameterIds.size(); ++index)
                    {
                        const float value = referenceParameters[index]->getValue();
                        liveParameters[index]->setValueNotifyingHost(value);
                        referenceParameters[index]->setValueNotifyingHost(value);
                    }
                    break;

                case Action::panic:
                    live.panic();
                    reference.panic();
                    break;

                case Action::bypass:
                    bypassed = true;
                    break;

                case Action::resume:
                    bypassed = false;
                    break;

                case Action::reset:
                    live.reset();
                    reference.reset();
                    break;

                case Action::apply:
                    REQUIRE_FALSE(live.applyPatch(decodeLive(next.text)).has_value());
                    REQUIRE_FALSE(reference.applyPatch(decodeReference(next.text)).has_value());
                    break;

                case Action::restore:
                    restore(live, saved.text);
                    restore(reference, saved.text);
                    break;

                case Action::reprepare:
                {
                    const double otherRate = sampleRate == 22050.0 ? 96000.0 : 22050.0;
                    prepare(live, otherRate, blockSize);
                    prepare(reference, otherRate, blockSize);
                    break;
                }
            }
        }

        const auto renderBlock = [&](juce::AudioProcessor& processor, juce::AudioBuffer<float>& output) {
            block.setSize(2, length, false, false, true);

            for (int channel = 0; channel < 2; ++channel)
                juce::FloatVectorOperations::fill(block.getWritePointer(channel), std::numeric_limits<float>::quiet_NaN(), length);

            midi.clear();

            for (auto event = nextEvent; event < script.size() && script[event].sampleTime < start + length; ++event)
                midi.addEvent(script[event].bytes.data(), script[event].size, script[event].sampleTime - start);

            if (bypassed)
                processor.processBlockBypassed(block, midi);
            else
                processor.processBlock(block, midi);

            for (int channel = 0; channel < 2; ++channel)
                output.copyFrom(channel, start, block, channel, 0, length);
        };

        renderBlock(live, renders.live);
        renderBlock(reference, renders.reference);

        while (nextEvent < script.size() && script[nextEvent].sampleTime < start + length)
            ++nextEvent;
    }

    return renders;
}

/** Records the digest of a render in the file named by COMPOSER_REFERENCE_RENDER_HASHES, when that
    is set, to compare renders between machines. Nothing checks it. */
void recordDigest(const std::string& label, const juce::AudioBuffer<float>& output)
{
    const auto path = juce::SystemStats::getEnvironmentVariable("COMPOSER_REFERENCE_RENDER_HASHES", {});

    if (path.isEmpty())
        return;

    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(output.getNumChannels() * output.getNumSamples()));

    for (int channel = 0; channel < output.getNumChannels(); ++channel)
        samples.insert(samples.end(), output.getReadPointer(channel), output.getReadPointer(channel) + output.getNumSamples());

    std::ofstream file(std::filesystem::path(path.toWideCharPointer()), std::ios::app);
    file << label << ' ' << sha256Hex(samples.data(), samples.size() * sizeof(float)) << '\n';
}

template <typename Value>
auto bitsOf(Value value)
{
    if constexpr (sizeof(Value) == 8)
        return std::bit_cast<std::uint64_t>(value);
    else
        return std::bit_cast<std::uint32_t>(value);
}

/** The first way the live synthesiser's state differs from the frozen one's, if any: voice
    counts, the filter's effective cutoff and every voice's stage, note, envelope level, start
    order and waveform, with doubles compared bit for bit. */
std::optional<std::string> stateDifference(const liveSynth::Synth& current, const v1Synth::Synth& frozen)
{
    std::ostringstream report;
    report << std::setprecision(17);

    if (current.activeVoiceCount() != frozen.activeVoiceCount() || current.fadingVoiceCount() != frozen.fadingVoiceCount())
    {
        report << "active and fading voices " << current.activeVoiceCount() << ' ' << current.fadingVoiceCount()
               << " against the reference's " << frozen.activeVoiceCount() << ' ' << frozen.fadingVoiceCount();
        return report.str();
    }

    if (bitsOf(current.effectiveCutoffHz()) != bitsOf(frozen.effectiveCutoffHz()))
    {
        report << "effective cutoff " << current.effectiveCutoffHz() << " Hz against the reference's "
               << frozen.effectiveCutoffHz() << " Hz";
        return report.str();
    }

    const auto currentVoices = current.voices();
    const auto frozenVoices = frozen.voices();

    if (currentVoices.size() != frozenVoices.size())
    {
        report << currentVoices.size() << " voices against the reference's " << frozenVoices.size();
        return report.str();
    }

    for (std::size_t slot = 0; slot < currentVoices.size(); ++slot)
    {
        const auto& a = currentVoices[slot];
        const auto& b = frozenVoices[slot];

        if (static_cast<int>(a.stage) != static_cast<int>(b.stage) || a.channel != b.channel || a.note != b.note
            || bitsOf(a.level) != bitsOf(b.level) || a.startOrder != b.startOrder
            || static_cast<int>(a.waveform) != static_cast<int>(b.waveform))
        {
            report << "voice " << slot << ": stage " << static_cast<int>(a.stage) << ", channel " << a.channel << ", note "
                   << a.note << ", level " << a.level << ", start " << a.startOrder << ", waveform "
                   << static_cast<int>(a.waveform) << " against the reference's stage " << static_cast<int>(b.stage)
                   << ", channel " << b.channel << ", note " << b.note << ", level " << b.level << ", start "
                   << b.startOrder << ", waveform " << static_cast<int>(b.waveform);
            return report.str();
        }
    }

    return std::nullopt;
}

/** A synthesiser core with its patch state and pending fade, driven as the processor drives it. */
template <typename PatchState, typename Synth, typename Patch>
struct Core
{
    explicit Core(const Patch& patch)
        : state(patch)
    {
    }

    /** What the processor does at the start of a block: read the patch and take a pending fade. */
    void startBlock() noexcept
    {
        if (Patch targets; state.tryRead(targets))
            synth.setTargets(targets);

        if (std::exchange(fadeRequested, false))
            synth.fadeOutAllVoices();
    }

    PatchState state;
    Synth synth;
    bool fadeRequested = false;
};

using LiveCore = Core<liveSynth::PatchState, liveSynth::Synth, composer::contracts::InstrumentPatch>;
using ReferenceCore = Core<v1Synth::PatchState, v1Synth::Synth, composer_v1::contracts::InstrumentPatch>;

} // namespace

TEST_CASE("Every saved v1 patch is accepted as saved by the current instrument")
{
    const auto saved = loadSavedV1Patches();
    REQUIRE(saved.size() == 5);

    for (const auto& [name, text] : saved)
    {
        INFO(name << ": " << text);

        // The saved text is exactly what the v1 encoder writes for the patch it holds.
        const auto encoded = composer_v1::contracts::encodePatch(decodeReference(text));
        REQUIRE(std::holds_alternative<std::string>(encoded));
        CHECK(std::get<std::string>(encoded) == text);

        CHECK(std::holds_alternative<composer::contracts::InstrumentPatch>(composer::contracts::decodePatch(text)));
    }
}

TEST_CASE("The instrument renders every saved v1 patch bit for bit like the frozen v1 reference")
{
    const auto saved = loadSavedV1Patches();
    REQUIRE(saved.size() == 5);

    for (std::size_t patchIndex = 0; patchIndex < saved.size(); ++patchIndex)
    {
        const auto& patch = saved[patchIndex];
        const auto& next = saved[(patchIndex + 1) % saved.size()];

        // The live processor ignores a state it cannot decode, so the render would not show it.
        decodeLive(patch.text);

        for (const auto& [rate, blockSize] : settings)
        {
            for (const auto performance : { Performance::standard, Performance::automation, Performance::lifecycle })
            {
                INFO(patch.name << " at " << rate << " Hz in blocks of " << blockSize << " with the " << nameOf(performance)
                                << " performance");

                const auto [liveOutput, referenceOutput] = renderBoth(patch, next, performance, rate, blockSize);
                const int total = liveOutput.getNumSamples();
                const auto difference = firstBitDifference(liveOutput, referenceOutput);
                std::ostringstream report;
                report << std::setprecision(9);

                if (difference.has_value())
                {
                    const int channel = *difference / total;
                    const int sample = *difference % total;
                    const float liveSample = liveOutput.getSample(channel, sample);
                    const float referenceSample = referenceOutput.getSample(channel, sample);
                    report << "channel " << channel << ", sample " << sample << ": " << liveSample << " (bits "
                           << bitsOf(liveSample) << ") against the reference's " << referenceSample << " (bits "
                           << bitsOf(referenceSample) << ")";
                }

                INFO("first difference: " << report.str());
                CHECK_FALSE(difference.has_value());

                CHECK(allFinite(liveOutput));

                if (patch.name != "lower_bounds" || performance != Performance::standard)
                    CHECK(peak(liveOutput) > 0.01f);

                recordDigest(patch.name + " " + std::to_string(static_cast<int>(rate)) + " " + std::to_string(blockSize) + " "
                                 + nameOf(performance),
                             liveOutput);
            }
        }
    }
}

TEST_CASE("The synthesiser core keeps every voice's double-precision state bit for bit like the frozen v1 core")
{
    // The float output can hide a last-bit change in the double-precision state behind it, so this
    // drives the two cores sample by sample, as the processors drive them, and compares their state
    // after every sample.
    const auto saved = loadSavedV1Patches();
    const auto indices = liveIndices();

    for (const auto& patch : saved)
    {
        const auto livePatch = decodeLive(patch.text);
        const auto referencePatch = decodeReference(patch.text);

        for (const auto& [rate, blockSize] : settings)
        {
            for (const auto performance : { Performance::standard, Performance::automation })
            {
                INFO(patch.name << " at " << rate << " Hz in blocks of " << blockSize << " with the " << nameOf(performance)
                                << " performance");

                const auto script = scriptFor(performance, rate);
                const auto& steps = stepsFor(performance);
                const int total = static_cast<int>(2.5 * rate);

                LiveCore current(livePatch);
                ReferenceCore frozen(referencePatch);
                current.synth.prepare(rate, current.state.snapshot());
                frozen.synth.prepare(rate, frozen.state.snapshot());

                std::size_t nextEvent = 0;
                std::size_t nextStep = 0;
                std::optional<std::string> difference;

                for (int start = 0; start < total && ! difference.has_value(); start += blockSize)
                {
                    const int end = std::min(start + blockSize, total);

                    for (; nextStep < steps.size() && static_cast<int>(std::floor(steps[nextStep].seconds * rate)) < end; ++nextStep)
                    {
                        const auto& step = steps[nextStep];

                        if (step.action == Action::set)
                        {
                            const auto parameter = static_cast<std::size_t>(step.parameter);
                            current.state.setHostValue(indices[parameter], step.value);
                            frozen.state.setHostValue(parameter, step.value);
                        }
                        else if (step.action == Action::echoAll)
                        {
                            for (std::size_t parameter = 0; parameter < v1ParameterIds.size(); ++parameter)
                            {
                                const float value = frozen.state.hostValue(parameter);
                                current.state.setHostValue(indices[parameter], value);
                                frozen.state.setHostValue(parameter, value);
                            }
                        }
                        else
                        {
                            REQUIRE(step.action == Action::panic);
                            current.fadeRequested = true;
                            frozen.fadeRequested = true;
                        }
                    }

                    // The processors render with denormals flushed to zero.
                    const juce::ScopedNoDenormals noDenormals;
                    current.startBlock();
                    frozen.startBlock();

                    for (int sample = start; sample < end && ! difference.has_value(); ++sample)
                    {
                        for (; nextEvent < script.size() && script[nextEvent].sampleTime <= sample; ++nextEvent)
                        {
                            const auto& event = script[nextEvent];
                            current.synth.handleMidi(event.bytes.data(), event.size);
                            frozen.synth.handleMidi(event.bytes.data(), event.size);
                        }

                        float currentOutput = 0.0f;
                        float frozenOutput = 0.0f;
                        current.synth.render(&currentOutput, 1);
                        frozen.synth.render(&frozenOutput, 1);

                        if (bitsOf(currentOutput) != bitsOf(frozenOutput))
                        {
                            std::ostringstream report;
                            report << std::setprecision(9) << "output " << currentOutput << " against the reference's "
                                   << frozenOutput;
                            difference = report.str();
                        }
                        else
                        {
                            difference = stateDifference(current.synth, frozen.synth);
                        }

                        if (difference.has_value())
                            difference = "sample " + std::to_string(sample) + ": " + *difference;
                    }
                }

                INFO("first difference: " << difference.value_or("none"));
                CHECK_FALSE(difference.has_value());
            }
        }
    }
}
