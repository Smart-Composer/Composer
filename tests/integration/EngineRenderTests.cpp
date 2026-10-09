#include <composer/engine/EngineSetup.h>

#include <catch2/catch_test_macros.hpp>
#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <cmath>

namespace
{

/** A directory that exists for the lifetime of one test and is removed afterwards. */
class ScratchDirectory
{
public:
    ScratchDirectory()
        : directory(juce::File::createTempFile("composer-test"))
    {
        REQUIRE(directory.createDirectory());
    }

    ~ScratchDirectory()
    {
        directory.deleteRecursively();
    }

    const juce::File& get() const
    {
        return directory;
    }

private:
    juce::File directory;
};

} // namespace

TEST_CASE("The sequencing engine renders a MIDI clip through a built-in synth offline")
{
    const ScratchDirectory scratch;
    auto engine = composer::engine::createHeadlessEngine("ComposerTests", scratch.get());
    const auto output = scratch.get().getChildFile("render.wav");

    {
        auto edit = tracktion::Edit::createSingleTrackEdit(*engine, tracktion::Edit::EditRole::forRendering);
        auto tracks = tracktion::getAudioTracks(*edit);
        REQUIRE(tracks.size() == 1);
        auto& track = *tracks.getFirst();

        auto synth = edit->getPluginCache().createNewPlugin(tracktion::FourOscPlugin::xmlTypeName, {});
        REQUIRE(synth != nullptr);
        track.pluginList.insertPlugin(synth, 0, nullptr);

        const auto clipEnd = tracktion::TimePosition::fromSeconds(1.0);
        auto clip = track.insertMIDIClip({ tracktion::TimePosition(), clipEnd }, nullptr);
        REQUIRE(clip != nullptr);
        clip->getSequence().addNote(69,
                                    tracktion::BeatPosition::fromBeats(0.0),
                                    tracktion::BeatDuration::fromBeats(1.0),
                                    100,
                                    0,
                                    nullptr);

        REQUIRE(tracktion::Renderer::renderToFile(*edit, output, false));
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(output));
    REQUIRE(reader != nullptr);

    CHECK(reader->sampleRate > 0.0);
    CHECK(reader->numChannels == 2);
    REQUIRE(reader->lengthInSamples >= static_cast<juce::int64>(reader->sampleRate * 0.9));

    juce::AudioBuffer<float> rendered(static_cast<int>(reader->numChannels),
                                      static_cast<int>(reader->lengthInSamples));
    REQUIRE(reader->read(&rendered, 0, rendered.getNumSamples(), 0, true, true));

    bool allFinite = true;
    float peak = 0.0f;

    for (int channel = 0; channel < rendered.getNumChannels(); ++channel)
    {
        for (int index = 0; index < rendered.getNumSamples(); ++index)
        {
            const auto sample = rendered.getSample(channel, index);
            allFinite = allFinite && std::isfinite(sample);
            peak = std::max(peak, std::abs(sample));
        }
    }

    CHECK(allFinite);
    CHECK(peak > 0.01f);
    CHECK(peak <= 1.0f);
}
