#include "PatchFixtures.h"
#include "RenderHarness.h"

#include <composer/instrument/InstrumentProcessor.h>

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <cstring>
#include <string>
#include <variant>

namespace
{

using namespace composer;
using namespace composer::tests;
using composer::instrument::InstrumentProcessor;

/** A directory of its own under the system's temporary directory, removed afterwards. Its name
    includes a character outside the common Windows code pages, so the render host must receive
    its paths as Unicode. */
struct ScratchDirectory
{
    juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("composer-vst3-render-"
                                             + juce::String::charToString(static_cast<juce::juce_wchar>(0x0141))
                                             + "-" + juce::Uuid().toString());

    ScratchDirectory()
    {
        REQUIRE(directory.createDirectory());
    }

    ~ScratchDirectory()
    {
        directory.deleteRecursively();
    }
};

/** Renders a script through the VST3 in the render host built on Steinberg's SDK. */
juce::AudioBuffer<float> renderInSdkHost(const contracts::InstrumentPatch& patch,
                                         const MidiScript& script,
                                         double sampleRate,
                                         int blockSize,
                                         int totalSamples)
{
    const ScratchDirectory scratch;
    const auto state = scratch.directory.getChildFile("state.json");
    const auto events = scratch.directory.getChildFile("events.txt");
    const auto output = scratch.directory.getChildFile("output.f32");

    const auto encoded = std::get<std::string>(contracts::encodePatch(patch));
    REQUIRE(state.replaceWithData(encoded.data(), encoded.size()));

    juce::String lines;
    for (const auto& event : script)
        lines << event.sampleTime << ' ' << static_cast<int>(event.bytes[0]) << ' ' << static_cast<int>(event.bytes[1]) << ' '
              << static_cast<int>(event.bytes[2]) << '\n';
    REQUIRE(events.replaceWithText(lines));

    juce::ChildProcess host;
    REQUIRE(host.start(juce::StringArray { COMPOSER_TEST_VST3_RENDER_HOST,
                                           juce::File(COMPOSER_TEST_VST3_BUNDLE).getFullPathName(),
                                           state.getFullPathName(),
                                           events.getFullPathName(),
                                           juce::String(sampleRate),
                                           juce::String(blockSize),
                                           juce::String(totalSamples),
                                           output.getFullPathName() }));

    // The host writes at most a line, which the pipe holds, so it can finish before its output is
    // read; reading first would wait for it to exit with no time limit.
    const bool finished = host.waitForProcessToFinish(60000);

    if (! finished)
        host.kill();

    const auto messages = host.readAllProcessOutput();
    INFO(messages.toStdString());
    REQUIRE(finished);
    REQUIRE(host.getExitCode() == 0);

    juce::MemoryBlock bytes;
    REQUIRE(output.loadFileAsData(bytes));
    REQUIRE(bytes.getSize() == static_cast<std::size_t>(2 * totalSamples) * sizeof(float));

    juce::AudioBuffer<float> rendered(2, totalSamples);
    for (int channel = 0; channel < 2; ++channel)
        std::memcpy(rendered.getWritePointer(channel),
                    static_cast<const char*>(bytes.getData()) + static_cast<std::size_t>(channel * totalSamples) * sizeof(float),
                    static_cast<std::size_t>(totalSamples) * sizeof(float));

    return rendered;
}

} // namespace

TEST_CASE("A host built on Steinberg's SDK renders every fixture patch like the in-process processor")
{
    auto patches = loadPatchFixtures();
    patches.emplace_back("awkward", awkwardPatch());

    for (const auto& [name, patch] : patches)
    {
        for (const auto& [rate, block] : { std::pair { 48000.0, 128 }, std::pair { 44100.0, 512 } })
        {
            INFO(name << " at " << rate << " Hz in blocks of " << block);
            const auto script = standardScript(rate);
            const int total = static_cast<int>(2.5 * rate);

            InstrumentProcessor local;
            REQUIRE_FALSE(local.applyPatch(patch).has_value());
            local.setRateAndBufferSizeDetails(rate, block);
            local.prepareToPlay(rate, block);
            const auto expected = renderScript(local, script, total, block);

            const auto rendered = renderInSdkHost(patch, script, rate, block, total);
            const auto difference = firstBitDifference(rendered, expected);

            {
                INFO("first difference at flattened sample " << difference.value_or(0));
                CHECK_FALSE(difference.has_value());
            }

            if (name != "lower_bounds")
                CHECK(peak(rendered) > 0.01f);
        }
    }
}
