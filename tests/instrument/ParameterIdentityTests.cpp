#include "ReferencePatches.h"
#include "Vst3ReservedParameterIds.h"

#include <composer/instrument/InstrumentProcessor.h>
#include <composer/instrument/synth/ParameterCurves.h>
#include <composer_v1/instrument/InstrumentProcessor.h>
#include <composer_v1/instrument/synth/ParameterCurves.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace
{

using Json = nlohmann::json;
using LiveProcessor = composer::instrument::InstrumentProcessor;
using ReferenceProcessor = composer_v1::instrument::InstrumentProcessor;

constexpr int longText = 1024;

std::uint32_t vst3IdOf(const juce::String& id)
{
    return juce::VST3ClientExtensions::convertJuceParameterId(id, true);
}

/** What the plugin wrapper and hosts read from each parameter: identity, the attributes the VST3
    wrapper turns into parameter flags and units, steps and the text of five normalised values. */
Json describeParameters(juce::AudioProcessor& processor)
{
    auto parameters = Json::array();
    int index = 0;

    for (auto* parameter : processor.getParameters())
    {
        const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*>(parameter);
        REQUIRE(withId != nullptr);

        auto texts = Json::object();
        for (const auto& [key, value] : { std::pair { "0", 0.0f }, std::pair { "0.25", 0.25f }, std::pair { "0.5", 0.5f },
                                          std::pair { "0.75", 0.75f }, std::pair { "1", 1.0f } })
            texts[key] = parameter->getText(value, longText).toStdString();

        // The wrapper gives each group a unit of its own.
        std::string group;
        for (const auto* parent : processor.getParameterTree().getGroupsForParameter(parameter))
            group += "/" + parent->getID().toStdString();

        parameters.push_back({
            { "index", index++ },
            { "id", withId->paramID.toStdString() },
            { "vst3_id", vst3IdOf(withId->paramID) },
            { "name", parameter->getName(longText).toStdString() },
            { "label", parameter->getLabel().toStdString() },
            { "version_hint", parameter->getVersionHint() },
            { "group", group },
            { "category", static_cast<int>(parameter->getCategory()) },
            { "meta", parameter->isMetaParameter() },
            { "default_normalised", parameter->getDefaultValue() },
            { "discrete", parameter->isDiscrete() },
            { "boolean", parameter->isBoolean() },
            { "steps", parameter->getNumSteps() },
            { "automatable", parameter->isAutomatable() },
            { "text", texts },
        });
    }

    return { { "parameters", parameters } };
}

Json recordedV1Parameters()
{
    std::ifstream file(std::string(COMPOSER_SOURCE_DIR) + "/tests/instrument/fixtures/parameter-identity-v1.json");
    REQUIRE(file.good());
    return Json::parse(file);
}

/** The processor's parameter with an ID. */
juce::AudioProcessorParameter& parameterWithId(juce::AudioProcessor& processor, const std::string& id)
{
    juce::AudioProcessorParameter* found = nullptr;

    for (auto* parameter : processor.getParameters())
        if (const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*>(parameter);
            withId != nullptr && withId->paramID == juce::String(id))
            found = parameter;

    INFO("parameter " << id);
    REQUIRE(found != nullptr);
    return *found;
}

/** The descriptor index of a v1 parameter in the live and frozen contracts. */
std::pair<std::size_t, std::size_t> descriptorIndices(const std::string& id)
{
    const auto indexIn = [&id](const auto& descriptors) {
        const auto found = std::find_if(descriptors.begin(), descriptors.end(),
                                        [&id](const auto& descriptor) { return descriptor.id == id; });
        REQUIRE(found != descriptors.end());
        return static_cast<std::size_t>(found - descriptors.begin());
    };

    return { indexIn(composer::contracts::parameterDescriptors), indexIn(composer_v1::contracts::parameterDescriptors) };
}

std::vector<std::string> v1ParameterIds()
{
    std::vector<std::string> ids;

    for (const auto& descriptor : composer_v1::contracts::parameterDescriptors)
        ids.emplace_back(descriptor.id);

    return ids;
}

/** Normalised values: a grid of 4097, 2000 seeded random values, the floats next to each end and
    values outside the range. */
std::vector<float> normalisedValues()
{
    std::vector<float> values;

    for (int step = 0; step <= 4096; ++step)
        values.push_back(static_cast<float>(step) / 4096.0f);

    std::mt19937 random(5u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    for (int index = 0; index < 2000; ++index)
        values.push_back(unit(random));

    values.insert(values.end(), { std::nextafter(0.0f, 1.0f), std::nextafter(1.0f, 0.0f), -0.5f, 1.5f });
    return values;
}

} // namespace

TEST_CASE("Host parameters keep the identity and text they had in v1")
{
    const auto expected = recordedV1Parameters();

    // Floats are stored as the shortest decimal that reads back as the same double, so the
    // comparisons are exact. The record must describe the frozen v1 instrument exactly.
    ReferenceProcessor reference;
    const auto frozen = describeParameters(reference);
    INFO("the frozen v1 processor's parameters:\n" << frozen.dump(2));
    CHECK(frozen == expected);

    // A later version may append parameters, but every v1 parameter keeps its place and attributes.
    LiveProcessor processor;
    const auto actual = describeParameters(processor);
    INFO("the processor's parameters:\n" << actual.dump(2));

    const auto& expectedParameters = expected.at("parameters");
    const auto& actualParameters = actual.at("parameters");
    REQUIRE(actualParameters.size() >= expectedParameters.size());

    for (std::size_t index = 0; index < expectedParameters.size(); ++index)
    {
        INFO("parameter " << index);
        CHECK(actualParameters[index] == expectedParameters[index]);
    }
}

TEST_CASE("No patch parameter takes a VST3 parameter ID the plugin wrapper reserves")
{
    LiveProcessor processor;
    std::set<std::uint32_t> ids;

    for (auto* parameter : processor.getParameters())
    {
        const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*>(parameter);
        REQUIRE(withId != nullptr);
        const auto id = vst3IdOf(withId->paramID);
        INFO(withId->paramID << " as " << id);

        CHECK_FALSE(composer::tests::vst3::isReservedParameterId(id));
        CHECK(ids.insert(id).second);
    }

    CHECK(ids.size() == composer::contracts::parameterDescriptors.size());
}

TEST_CASE("Host values and parameter text match the frozen v1 instrument at every setting")
{
    // A host stores automation as normalised values and replays them; the instrument keeps a
    // patch's exact value when the host writes back that value's own normalised setting. Both
    // depend on these mappings matching v1 everywhere, not only at the defaults.
    const auto ids = v1ParameterIds();

    for (const auto& [name, text] : composer::tests::loadSavedV1Patches())
    {
        INFO("saved patch " << name);
        LiveProcessor processor;
        ReferenceProcessor reference;
        processor.setStateInformation(text.data(), static_cast<int>(text.size()));
        reference.setStateInformation(text.data(), static_cast<int>(text.size()));

        for (const auto& id : ids)
        {
            INFO("parameter " << id);
            CHECK(std::bit_cast<std::uint32_t>(parameterWithId(processor, id).getValue())
                  == std::bit_cast<std::uint32_t>(parameterWithId(reference, id).getValue()));
        }
    }

    LiveProcessor processor;
    ReferenceProcessor reference;
    const auto values = normalisedValues();

    for (const auto& id : ids)
    {
        INFO("parameter " << id);
        const auto& live = parameterWithId(processor, id);
        const auto& frozen = parameterWithId(reference, id);
        const auto [liveIndex, frozenIndex] = descriptorIndices(id);
        bool same = true;

        for (const float normalised : values)
        {
            const auto liveText = live.getText(normalised, 128);
            const auto frozenText = frozen.getText(normalised, 128);
            const double livePlain = composer::instrument::synth::denormalise(liveIndex, normalised);
            const double frozenPlain = composer_v1::instrument::synth::denormalise(frozenIndex, normalised);
            const float liveRoundTrip = composer::instrument::synth::normalise(liveIndex, frozenPlain);
            const float frozenRoundTrip = composer_v1::instrument::synth::normalise(frozenIndex, frozenPlain);

            if (liveText != frozenText
                || std::bit_cast<std::uint32_t>(live.getValueForText(frozenText))
                       != std::bit_cast<std::uint32_t>(frozen.getValueForText(frozenText))
                || std::bit_cast<std::uint64_t>(livePlain) != std::bit_cast<std::uint64_t>(frozenPlain)
                || std::bit_cast<std::uint32_t>(liveRoundTrip) != std::bit_cast<std::uint32_t>(frozenRoundTrip))
            {
                UNSCOPED_INFO("normalised " << normalised << ": text " << liveText << " against " << frozenText
                                            << ", value " << livePlain << " against " << frozenPlain);
                same = false;
                break;
            }
        }

        // Plain values across the v1 range, each normalised by both.
        const auto& descriptor = composer_v1::contracts::parameterDescriptors[frozenIndex];

        for (int step = 0; step <= 4096 && same; ++step)
        {
            const double plain = descriptor.minimum + (descriptor.maximum - descriptor.minimum) * step / 4096.0;
            const float liveNormalised = composer::instrument::synth::normalise(liveIndex, plain);
            const float frozenNormalised = composer_v1::instrument::synth::normalise(frozenIndex, plain);

            if (std::bit_cast<std::uint32_t>(liveNormalised) != std::bit_cast<std::uint32_t>(frozenNormalised))
            {
                UNSCOPED_INFO("plain " << plain << ": " << liveNormalised << " against " << frozenNormalised);
                same = false;
            }
        }

        CHECK(same);
    }
}
