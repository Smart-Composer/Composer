#include "PatchFixtures.h"
#include "RenderHarness.h"

#include <composer/instrument/InstrumentProcessor.h>
#include <composer/instrument/synth/ParameterCurves.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <limits>
#include <random>
#include <utility>
#include <variant>

namespace
{

using namespace composer;
using composer::instrument::InstrumentProcessor;
namespace synth = composer::instrument::synth;

/** Records every value and gesture notification of a processor's parameters. */
struct NotificationLog final : juce::AudioProcessorParameter::Listener
{
    std::vector<std::pair<int, float>> values;
    std::vector<std::pair<int, bool>> gestures;

    void parameterValueChanged(int index, float value) override
    {
        values.emplace_back(index, value);
    }

    void parameterGestureChanged(int index, bool starting) override
    {
        gestures.emplace_back(index, starting);
    }

    std::size_t total() const
    {
        return values.size() + gestures.size();
    }

    void clear()
    {
        values.clear();
        gestures.clear();
    }
};

struct Fixture
{
    InstrumentProcessor processor;
    NotificationLog log;

    Fixture()
    {
        for (auto* parameter : processor.getParameters())
            parameter->addListener(&log);

        processor.setRateAndBufferSizeDetails(48000.0, 128);
        processor.prepareToPlay(48000.0, 128);
    }

    ~Fixture()
    {
        for (auto* parameter : processor.getParameters())
            parameter->removeListener(&log);
    }
};

std::uint64_t bits(double value)
{
    return std::bit_cast<std::uint64_t>(value);
}

void requireExactly(const contracts::InstrumentPatch& actual, const contracts::InstrumentPatch& expected)
{
    for (std::size_t index = 0; index < synth::parameterCount; ++index)
    {
        INFO(contracts::parameterDescriptors[index].id);
        REQUIRE(bits(synth::fieldValue(actual, index)) == bits(synth::fieldValue(expected, index)));
    }
}

void requireHostValuesMatch(InstrumentProcessor& processor, const contracts::InstrumentPatch& patch)
{
    const auto& parameters = processor.getParameters();

    for (std::size_t index = 0; index < synth::parameterCount; ++index)
        REQUIRE(parameters[static_cast<int>(index)]->getValue() == synth::normalise(index, synth::fieldValue(patch, index)));
}

contracts::InstrumentPatch randomPatch(std::mt19937& random)
{
    contracts::InstrumentPatch patch;
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    for (std::size_t index = 0; index < synth::parameterCount; ++index)
    {
        const auto& descriptor = contracts::parameterDescriptors[index];
        const double value = descriptor.kind == contracts::ParameterKind::choice
                               ? std::floor(unit(random) * 3.0)
                               : descriptor.minimum + (descriptor.maximum - descriptor.minimum) * unit(random);
        synth::setFieldValue(patch, index, std::min(value, descriptor.maximum));
    }

    return patch;
}

juce::AudioBuffer<float> renderShort(InstrumentProcessor& processor)
{
    return composer::tests::renderScript(processor,
                                         { composer::tests::noteOn(0, 60, 100), composer::tests::noteOn(64, 67, 80) },
                                         2048, 128);
}

std::string stateOf(InstrumentProcessor& processor)
{
    juce::MemoryBlock block;
    processor.getStateInformation(block);
    return std::string(static_cast<const char*>(block.getData()), block.getSize());
}

std::string encoded(const contracts::InstrumentPatch& patch)
{
    return std::get<std::string>(contracts::encodePatch(patch));
}

} // namespace

TEST_CASE("Host parameters mirror the patch descriptors with persistent identities")
{
    InstrumentProcessor processor;
    const auto& parameters = processor.getParameters();
    REQUIRE(parameters.size() == static_cast<int>(synth::parameterCount));

    // VST3 parameter IDs derived from the parameter IDs; saved automation depends on them.
    const std::array<std::uint32_t, 8> vst3Ids { 604207933u, 1948451134u, 42801032u, 1860120314u,
                                                 1770322536u, 1899307591u, 199910052u, 987871424u };

    for (std::size_t index = 0; index < synth::parameterCount; ++index)
    {
        const auto& descriptor = contracts::parameterDescriptors[index];
        INFO(descriptor.id);
        auto* parameter = dynamic_cast<juce::HostedAudioProcessorParameter*>(parameters[static_cast<int>(index)]);
        REQUIRE(parameter != nullptr);

        const auto id = parameter->getParameterID();
        CHECK(id == juce::String(descriptor.id.data(), descriptor.id.size()));
        CHECK(parameter->getName(100) == juce::String(descriptor.displayName.data(), descriptor.displayName.size()));
        CHECK(parameter->getLabel() == juce::String(descriptor.unit.data(), descriptor.unit.size()));
        CHECK(parameter->getVersionHint() == 1);
        CHECK(parameter->getDefaultValue() == synth::normalise(index, descriptor.defaultValue));
        CHECK(parameter->isAutomatable());
        CHECK(juce::VST3ClientExtensions::convertJuceParameterId(id, true) == vst3Ids[index]);

        const bool choice = descriptor.kind == contracts::ParameterKind::choice;
        CHECK(parameter->isDiscrete() == choice);

        if (choice)
        {
            CHECK(parameter->getNumSteps() == 3);
            CHECK(parameter->getText(0.0f, 100) == "Sine");
            CHECK(parameter->getText(0.5f, 100) == "Saw");
            CHECK(parameter->getText(1.0f, 100) == "Square");
            CHECK(parameter->getValueForText("square") == 1.0f);
        }
    }

    // Values display in their unit's precision: whole hertz, tenths of a decibel.
    const auto textOf = [&](std::string_view id, double value) {
        std::size_t index = 0;
        while (contracts::parameterDescriptors[index].id != id)
            ++index;
        return parameters[static_cast<int>(index)]->getText(synth::normalise(index, value), 100);
    };

    CHECK(textOf("cutoff_hz", 8000.0) == "8000");
    CHECK(textOf("cutoff_hz", 1234.5678901) == "1235");
    CHECK(textOf("gain_db", -12.0) == "-12.0");
    CHECK(textOf("resonance_q", 0.7071067811865476) == "0.71");
}

TEST_CASE("An applied patch is reported back exactly and notified once per changed parameter")
{
    Fixture fixture;
    auto& processor = fixture.processor;
    std::mt19937 random(424242u);

    std::vector<contracts::InstrumentPatch> patches;
    for (const auto& [name, patch] : composer::tests::loadPatchFixtures())
        patches.push_back(patch);
    patches.push_back(composer::tests::awkwardPatch());
    for (int index = 0; index < 1000; ++index)
        patches.push_back(randomPatch(random));

    for (const auto& patch : patches)
    {
        const auto before = processor.currentPatch();
        fixture.log.clear();

        REQUIRE_FALSE(processor.applyPatch(patch).has_value());
        requireExactly(processor.currentPatch(), patch);
        requireHostValuesMatch(processor, patch);

        std::size_t changed = 0;
        for (std::size_t index = 0; index < synth::parameterCount; ++index)
            changed += synth::normalise(index, synth::fieldValue(before, index))
                           != synth::normalise(index, synth::fieldValue(patch, index)) ? 1u : 0u;

        REQUIRE(fixture.log.values.size() == changed);
        REQUIRE(fixture.log.gestures.size() == 2 * changed);

        fixture.log.clear();
        REQUIRE_FALSE(processor.applyPatch(patch).has_value());
        REQUIRE(fixture.log.total() == 0);
    }

    const auto last = patches.back();
    renderShort(processor);

    for (auto* parameter : processor.getParameters())
    {
        parameter->setValue(parameter->getValue());
        parameter->setValueNotifyingHost(parameter->getValue());
    }

    requireExactly(processor.currentPatch(), last);
}

TEST_CASE("The host hears each changed parameter's latest value, and keeps an edit it makes meanwhile")
{
    Fixture fixture;
    auto& processor = fixture.processor;
    REQUIRE_FALSE(processor.applyPatch(composer::tests::awkwardPatch()).has_value());

    std::size_t gain = 0;
    std::size_t cutoff = 0;
    while (contracts::parameterDescriptors[gain].id != "gain_db")
        ++gain;
    while (contracts::parameterDescriptors[cutoff].id != "cutoff_hz")
        ++cutoff;
    REQUIRE(gain < cutoff);

    // A host that moves the cutoff as soon as it is told the gain changed, before it is told
    // about the cutoff.
    struct EditingHost final : juce::AudioProcessorParameter::Listener
    {
        juce::AudioProcessorParameter* cutoff = nullptr;
        bool edited = false;

        void parameterValueChanged(int, float) override
        {
            if (! std::exchange(edited, true))
                cutoff->setValue(0.25f);
        }

        void parameterGestureChanged(int, bool) override
        {
        }
    };

    auto& parameters = processor.getParameters();
    EditingHost host;
    host.cutoff = parameters[static_cast<int>(cutoff)];
    parameters[static_cast<int>(gain)]->addListener(&host);

    auto patch = composer::tests::awkwardPatch();
    patch.gainDb = -3.0;
    patch.cutoffHz = 5000.0;
    fixture.log.clear();
    REQUIRE_FALSE(processor.applyPatch(patch).has_value());
    parameters[static_cast<int>(gain)]->removeListener(&host);

    auto expected = patch;
    expected.cutoffHz = synth::denormalise(cutoff, 0.25f);
    requireExactly(processor.currentPatch(), expected);

    REQUIRE(fixture.log.values.size() == 2);
    CHECK(fixture.log.values[0] == std::pair { static_cast<int>(gain), synth::normalise(gain, -3.0) });
    CHECK(fixture.log.values[1] == std::pair { static_cast<int>(cutoff), 0.25f });
}

TEST_CASE("An invalid patch is rejected without any change")
{
    Fixture fixture;
    Fixture control;
    REQUIRE_FALSE(fixture.processor.applyPatch(composer::tests::awkwardPatch()).has_value());
    REQUIRE_FALSE(control.processor.applyPatch(composer::tests::awkwardPatch()).has_value());

    const auto expected = fixture.processor.currentPatch();
    fixture.log.clear();

    for (std::size_t index = 0; index < synth::parameterCount; ++index)
    {
        const auto& descriptor = contracts::parameterDescriptors[index];

        if (descriptor.kind == contracts::ParameterKind::choice)
            continue;

        for (const double bad : { std::nextafter(descriptor.minimum, -1.0e300), std::nextafter(descriptor.maximum, 1.0e300),
                                  std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity() })
        {
            auto patch = expected;
            synth::setFieldValue(patch, index, bad);
            const auto error = fixture.processor.applyPatch(patch);
            INFO(descriptor.id << " = " << bad);
            REQUIRE(error.has_value());
            CHECK(error->code == contracts::ErrorCode::invalidPatch);
            CHECK(error->field == std::string(descriptor.id));
        }
    }

    auto badWaveform = expected;
    badWaveform.waveform = static_cast<contracts::Waveform>(3);
    const auto error = fixture.processor.applyPatch(badWaveform);
    REQUIRE(error.has_value());
    CHECK(error->field == "waveform");

    requireExactly(fixture.processor.currentPatch(), expected);
    requireHostValuesMatch(fixture.processor, expected);
    CHECK(fixture.log.total() == 0);
    CHECK_FALSE(composer::tests::firstBitDifference(renderShort(fixture.processor), renderShort(control.processor)).has_value());
}

TEST_CASE("A host edit changes exactly one field to the host's value")
{
    Fixture fixture;
    auto& processor = fixture.processor;
    const auto patch = composer::tests::awkwardPatch();
    REQUIRE_FALSE(processor.applyPatch(patch).has_value());

    std::size_t cutoff = 0;
    while (contracts::parameterDescriptors[cutoff].id != "cutoff_hz")
        ++cutoff;

    auto* parameter = processor.getParameters()[static_cast<int>(cutoff)];
    parameter->setValue(0.25f);

    auto expected = patch;
    expected.cutoffHz = synth::denormalise(cutoff, 0.25f);
    requireExactly(processor.currentPatch(), expected);
    CHECK(parameter->getValue() == 0.25f);
    CHECK_FALSE(contracts::validatePatch(processor.currentPatch()).has_value());

    parameter->setValue(-1.0f);
    CHECK(processor.currentPatch().cutoffHz == 20.0);
    parameter->setValue(2.0f);
    CHECK(processor.currentPatch().cutoffHz == 20000.0);
    parameter->setValue(std::numeric_limits<float>::infinity());
    CHECK(processor.currentPatch().cutoffHz == 20000.0);
    parameter->setValue(0.5f);
    parameter->setValue(std::numeric_limits<float>::quiet_NaN());
    CHECK(processor.currentPatch().cutoffHz == synth::denormalise(cutoff, 0.5f));
}

TEST_CASE("The saved state is exactly the patch's contract JSON")
{
    Fixture fixture;
    auto& processor = fixture.processor;

    CHECK(stateOf(processor) == encoded(processor.currentPatch()));

    REQUIRE_FALSE(processor.applyPatch(composer::tests::awkwardPatch()).has_value());
    CHECK(stateOf(processor) == encoded(composer::tests::awkwardPatch()));

    processor.getParameters()[1]->setValue(0.4f);
    CHECK(stateOf(processor) == encoded(processor.currentPatch()));
}

TEST_CASE("Restoring a state plays that patch and notifies only what changed")
{
    for (const auto& [name, patch] : composer::tests::loadPatchFixtures())
    {
        INFO(name);
        Fixture restored;
        Fixture applied;
        const auto before = restored.processor.currentPatch();
        const auto bytes = encoded(patch);

        restored.log.clear();
        restored.processor.setStateInformation(bytes.data(), static_cast<int>(bytes.size()));
        requireExactly(restored.processor.currentPatch(), patch);
        CHECK(restored.log.gestures.empty());

        std::size_t changed = 0;
        for (std::size_t index = 0; index < synth::parameterCount; ++index)
            changed += synth::normalise(index, synth::fieldValue(before, index))
                           != synth::normalise(index, synth::fieldValue(patch, index)) ? 1u : 0u;
        CHECK(restored.log.values.size() == changed);

        REQUIRE_FALSE(applied.processor.applyPatch(patch).has_value());
        CHECK_FALSE(composer::tests::firstBitDifference(renderShort(restored.processor),
                                                        renderShort(applied.processor)).has_value());
    }
}

TEST_CASE("A state that does not decode is ignored")
{
    const auto valid = encoded(composer::tests::awkwardPatch());

    // A different, decodable patch padded beyond the state limit: accepting it would change the
    // patch, so the limit is observable.
    std::string padded = encoded(contracts::InstrumentPatch {});
    padded.insert(1, std::string(70 * 1024, ' '));

    std::mt19937 random(5u);
    std::string noise(512, '\0');
    for (auto& byte : noise)
        byte = static_cast<char>(random() & 0xFF);

    const std::vector<std::string> states {
        noise,
        valid.substr(0, valid.size() / 2),
        R"({"schema_version":2,"waveform":"sine","gain_db":-12,"attack_seconds":0.01,"decay_seconds":0.1,"sustain_level":0.7,"release_seconds":0.2,"cutoff_hz":8000,"resonance_q":0.7071067811865476})",
        R"({"schema_version":1,"waveform":"sine","gain_db":-12,"attack_seconds":0.01,"decay_seconds":0.1,"sustain_level":0.7,"release_seconds":0.2,"cutoff_hz":8000,"resonance_q":0.7071067811865476,"extra":1})",
        R"({"schema_version":1,"waveform":"sine","gain_db":-12,"attack_seconds":0.01,"decay_seconds":0.1,"sustain_level":0.7,"release_seconds":0.2,"cutoff_hz":80000,"resonance_q":0.7071067811865476})",
        R"({"schema_version":1,"waveform":"sine","waveform":"saw","gain_db":-12,"attack_seconds":0.01,"decay_seconds":0.1,"sustain_level":0.7,"release_seconds":0.2,"cutoff_hz":8000,"resonance_q":0.7071067811865476})",
        R"({"schema_version":1,"waveform":"sine","gain_db":NaN,"attack_seconds":0.01,"decay_seconds":0.1,"sustain_level":0.7,"release_seconds":0.2,"cutoff_hz":8000,"resonance_q":0.7071067811865476})",
        "<?xml version=\"1.0\"?><PARAMETERS gain=\"0.5\"/>",
        valid + "trailing",
        padded,
    };

    Fixture control;
    REQUIRE_FALSE(control.processor.applyPatch(composer::tests::awkwardPatch()).has_value());
    const auto reference = renderShort(control.processor);

    for (std::size_t index = 0; index <= states.size() + 1; ++index)
    {
        INFO("state " << index);
        Fixture fixture;
        REQUIRE_FALSE(fixture.processor.applyPatch(composer::tests::awkwardPatch()).has_value());
        fixture.log.clear();

        if (index < states.size())
            fixture.processor.setStateInformation(states[index].data(), static_cast<int>(states[index].size()));
        else if (index == states.size())
            fixture.processor.setStateInformation(nullptr, 64);
        else
            fixture.processor.setStateInformation(valid.data(), -1);

        requireExactly(fixture.processor.currentPatch(), composer::tests::awkwardPatch());
        requireHostValuesMatch(fixture.processor, composer::tests::awkwardPatch());
        CHECK(fixture.log.total() == 0);
        CHECK_FALSE(composer::tests::firstBitDifference(renderShort(fixture.processor), reference).has_value());
    }
}

TEST_CASE("A state carries an edited patch exactly to another instance")
{
    Fixture first;
    Fixture second;
    REQUIRE_FALSE(first.processor.applyPatch(composer::tests::awkwardPatch()).has_value());
    first.processor.getParameters()[1]->setValue(0.4f);

    juce::MemoryBlock state;
    first.processor.getStateInformation(state);
    second.processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    requireExactly(second.processor.currentPatch(), first.processor.currentPatch());

    for (int index = 0; index < static_cast<int>(synth::parameterCount); ++index)
        CHECK(std::bit_cast<std::uint32_t>(second.processor.getParameters()[index]->getValue())
              == std::bit_cast<std::uint32_t>(first.processor.getParameters()[index]->getValue()));

    CHECK_FALSE(composer::tests::firstBitDifference(renderShort(first.processor), renderShort(second.processor)).has_value());
}
