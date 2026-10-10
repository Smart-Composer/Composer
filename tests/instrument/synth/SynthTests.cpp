#include "SynthTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <tuple>

#include <xmmintrin.h>

namespace
{

using namespace composer;
using namespace composer::instrument::synth;
using namespace composer::instrument::synth::testing;

std::vector<Event> standardScript()
{
    return {
        noteOnAt(0, 60, 100),
        noteOnAt(311, 64, 64, 1),
        noteOnAt(700, 67, 127),
        noteOffAt(2400, 60),
        noteOnAt(2600, 60, 90),
        noteOffAt(5000, 64, 1),
        noteOnAt(5100, 72, 1),
        noteOffAt(9000, 67),
        noteOffAt(9500, 60),
        noteOffAt(12000, 72),
        noteOnAt(15000, 48, 110),
        noteOffAt(19000, 48),
    };
}

contracts::InstrumentPatch busyPatch()
{
    return makePatch(contracts::Waveform::saw, -6.0, 0.005, 0.08, 0.6, 0.05, 3000.0, 2.0);
}

int countVoices(const Synth& synth, VoiceStage stage)
{
    int count = 0;

    for (const auto& voice : synth.voices())
        count += voice.stage == stage ? 1 : 0;

    return count;
}

bool soundingNote(const Synth& synth, int note, int channel = 0)
{
    for (const auto& voice : synth.voices())
        if (voice.stage != VoiceStage::idle && voice.stage != VoiceStage::fading && voice.note == note
            && voice.channel == channel)
            return true;

    return false;
}

/** Sample-by-sample difference of two renders. */
std::vector<double> difference(const std::vector<float>& a, const std::vector<float>& b)
{
    std::vector<double> result(std::min(a.size(), b.size()));

    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = static_cast<double>(a[index]) - static_cast<double>(b[index]);

    return result;
}

/** A4 at -24 dB, held at full level: every voice playing it has the same, known level. */
contracts::InstrumentPatch unisonPatch()
{
    return makePatch(contracts::Waveform::sine, -24.0, 0.0, 0.0, 1.0, 10.0);
}

/** A4 on each of the first few channels, all in phase. */
std::vector<Event> unison(int channels)
{
    std::vector<Event> events;

    for (int channel = 0; channel < channels; ++channel)
        events.push_back(noteOnAt(0, 69, 127, channel));

    return events;
}

const double unisonVoice = std::pow(10.0, -24.0 / 20.0);
const double fadeLength = std::round(declickSeconds * 48000.0);

/** The level a velocity-1 note adds: next to nothing. */
const double quietNote = unisonVoice * std::pow(1.0 / 127.0, 2.0);

// A4 is a quarter cycle in, at a peak, here: a voice cut off at this sample steps by its full level.
constexpr int atPeak = 4827;

} // namespace

TEST_CASE("Voices are allocated, stolen and retriggered predictably")
{
    Synth synth;
    synth.prepare(48000.0, makePatch(contracts::Waveform::sine, 0.0, 0.01, 0.1, 0.8, 0.5));

    for (int note = 40; note < 56; ++note)
        synth.noteOn(0, note, 100);

    CHECK(synth.activeVoiceCount() == 16);
    CHECK(synth.fadingVoiceCount() == 0);
    synth.render(nullptr, 64);

    synth.noteOn(0, 70, 100);
    CHECK(synth.fadingVoiceCount() == 1);
    CHECK(synth.activeVoiceCount() == 16);
    CHECK_FALSE(soundingNote(synth, 40));
    CHECK(soundingNote(synth, 70));

    synth.render(nullptr, 1000);
    CHECK(synth.fadingVoiceCount() == 0);

    SECTION("a releasing voice is stolen before a held one")
    {
        synth.noteOff(0, 45);
        synth.render(nullptr, 10);
        synth.noteOn(0, 71, 100);

        CHECK_FALSE(soundingNote(synth, 45));
        CHECK(soundingNote(synth, 41));
        CHECK(countVoices(synth, VoiceStage::release) == 0);
    }

    SECTION("a repeated note retriggers in place")
    {
        const auto before = synth.voices();
        synth.noteOn(0, 50, 60);
        const auto after = synth.voices();

        for (std::size_t index = 0; index < before.size(); ++index)
        {
            if (before[index].note == 50 && before[index].stage != VoiceStage::idle)
            {
                CHECK(after[index].note == 50);
                CHECK(after[index].stage == VoiceStage::attack);
                CHECK(after[index].level == before[index].level);
            }
        }

        CHECK(synth.fadingVoiceCount() == 0);
    }

    SECTION("a repeated note with a new waveform fades the old voice and starts a new one")
    {
        synth.setTargets(makePatch(contracts::Waveform::square, 0.0, 0.01, 0.1, 0.8, 0.5));
        synth.noteOn(0, 50, 60);
        CHECK(synth.fadingVoiceCount() == 1);

        int squares = 0;
        for (const auto& voice : synth.voices())
            squares += voice.note == 50 && voice.waveform == contracts::Waveform::square ? 1 : 0;
        CHECK(squares == 1);
    }

    SECTION("channels are separate keys, velocity zero releases, and unknown note-offs do nothing")
    {
        synth.stopAllVoicesNow();
        synth.noteOn(0, 60, 100);
        synth.noteOn(1, 60, 100);
        CHECK(synth.activeVoiceCount() == 2);

        const std::array<std::uint8_t, 3> velocityZero { 0x90, 60, 0 };
        synth.handleMidi(velocityZero.data(), 3);
        CHECK(countVoices(synth, VoiceStage::release) == 1);

        synth.noteOff(5, 99);
        CHECK(synth.activeVoiceCount() == 2);
    }

    SECTION("a retriggered note counts as the newest when a note is taken")
    {
        // Note 41 is the oldest held note. Struck again, it becomes the newest, so the next
        // note takes note 42 instead.
        synth.noteOn(0, 41, 90);
        synth.noteOn(0, 90, 100);
        CHECK(soundingNote(synth, 41));
        CHECK_FALSE(soundingNote(synth, 42));
    }

    SECTION("a note started and taken in the same sample needs no fade")
    {
        for (int note = 80; note < 100; ++note)
            synth.noteOn(0, note, 100);

        // The sixteen held notes fade out; of the twenty new ones, the last sixteen sound.
        CHECK(synth.fadingVoiceCount() == 16);
        CHECK(synth.activeVoiceCount() == 16);

        for (int note = 80; note < 100; ++note)
            CHECK(soundingNote(synth, note) == (note >= 84));
    }
}

TEST_CASE("Every note taken at once fades out, held or releasing")
{
    constexpr double sampleRate = 48000.0;

    for (const bool releasing : { false, true })
    {
        for (const int taken : { 1, 4, 5, 8, 16 })
        {
            INFO((releasing ? "releasing" : "held") << ", " << taken << " notes taken at one sample");
            auto events = unison(16);

            if (releasing)
                for (int channel = 0; channel < 16; ++channel)
                    events.push_back(noteOffAt(2400, 69, channel));

            Synth reference;
            reference.prepare(sampleRate, unisonPatch());
            const auto untouched = renderEvents(reference, events, atPeak + 400);

            // The new notes are almost silent, so the difference is the taken notes' fades.
            for (int channel = 0; channel < taken; ++channel)
                events.push_back(noteOnAt(atPeak, 70, 1, channel));

            Synth synth;
            synth.prepare(sampleRate, unisonPatch());
            auto output = renderEvents(synth, events, atPeak);
            CHECK(synth.fadingVoiceCount() == taken);
            CHECK(synth.activeVoiceCount() == 16);

            output.resize(atPeak + 400);
            synth.render(output.data() + atPeak, 400);
            const auto change = difference(output, untouched);

            // A fade's first sample lowers a voice by 1/144 of its level; a voice cut off instead
            // would drop by all of it.
            CHECK(peak(untouched, 0, atPeak) > 0.5);
            CHECK(std::abs(change[atPeak]) <= taken * unisonVoice / fadeLength * 1.01 + taken * quietNote);
        }
    }
}

TEST_CASE("A waveform change restarts every repeated note with a fade")
{
    constexpr double sampleRate = 48000.0;
    const auto events = unison(8);

    Synth reference;
    reference.prepare(sampleRate, unisonPatch());
    const auto untouched = renderEvents(reference, events, atPeak + 400);

    Synth synth;
    synth.prepare(sampleRate, unisonPatch());
    auto output = renderEvents(synth, events, atPeak);
    auto square = unisonPatch();
    square.waveform = contracts::Waveform::square;
    synth.setTargets(square);

    for (int channel = 0; channel < 8; ++channel)
        synth.noteOn(channel, 69, 1);

    CHECK(synth.fadingVoiceCount() == 8);
    CHECK(synth.activeVoiceCount() == 8);
    output.resize(atPeak + 400);
    synth.render(output.data() + atPeak, 400);

    const auto change = difference(output, untouched);
    CHECK(std::abs(change[atPeak]) <= 8.0 * unisonVoice / fadeLength * 1.01 + 8.0 * quietNote);
}

TEST_CASE("A note that would need a fade while every voice is busy is ignored")
{
    constexpr double sampleRate = 48000.0;
    auto events = unison(16);

    // Sixteen new notes take the sixteen held ones, which fade out: every voice is then busy.
    for (int channel = 0; channel < 16; ++channel)
        events.push_back(noteOnAt(atPeak, 72, 100, channel));

    Synth reference;
    reference.prepare(sampleRate, unisonPatch());
    const auto expected = renderEvents(reference, events, atPeak + 1000);

    Synth synth;
    synth.prepare(sampleRate, unisonPatch());
    auto output = renderEvents(synth, events, atPeak + 48);
    REQUIRE(synth.fadingVoiceCount() == 16);
    REQUIRE(synth.activeVoiceCount() == 16);

    synth.noteOn(0, 90, 127);
    CHECK_FALSE(soundingNote(synth, 90));
    CHECK(synth.fadingVoiceCount() == 16);
    CHECK(synth.activeVoiceCount() == 16);

    output.resize(atPeak + 1000);
    synth.render(output.data() + atPeak + 48, 1000 - 48);
    CHECK_FALSE(firstBitDifference(output, expected).has_value());

    // Once the fades finish, a new note takes a held one again.
    synth.noteOn(0, 90, 127);
    CHECK(soundingNote(synth, 90));
    CHECK(synth.fadingVoiceCount() == 1);
}

TEST_CASE("A note that finds every voice busy just after a stop is ignored")
{
    // One note taken, then all sixteen stopped at once: seventeen voices are fading, so of a
    // sixteen-note chord within the next 3 ms only fifteen find a voice.
    Synth synth;
    synth.prepare(48000.0, unisonPatch());

    for (int note = 40; note < 56; ++note)
        synth.noteOn(0, note, 100);

    synth.render(nullptr, 64);
    synth.noteOn(0, 56, 100);
    synth.render(nullptr, 16);
    synth.allSoundOff(0);
    REQUIRE(synth.fadingVoiceCount() == 17);

    for (int note = 60; note < 76; ++note)
        synth.noteOn(0, note, 100);

    CHECK(synth.activeVoiceCount() == 15);
    CHECK_FALSE(soundingNote(synth, 75));

    synth.render(nullptr, 1000);
    CHECK(synth.fadingVoiceCount() == 0);
    CHECK(synth.activeVoiceCount() == 15);
}

TEST_CASE("A steal takes the quietest releasing note")
{
    Synth synth;
    synth.prepare(48000.0, makePatch(contracts::Waveform::sine, 0.0, 0.0, 0.0, 1.0, 1.0));

    for (int note = 40; note < 56; ++note)
        synth.noteOn(0, note, 100);

    // Note 42 has been releasing longest, so it is the quietest.
    synth.render(nullptr, 100);
    synth.noteOff(0, 42);
    synth.render(nullptr, 1900);
    synth.noteOff(0, 47);
    synth.render(nullptr, 100);

    synth.noteOn(0, 70, 100);
    CHECK_FALSE(soundingNote(synth, 42));
    CHECK(soundingNote(synth, 47));
    CHECK(soundingNote(synth, 70));
}

TEST_CASE("A repeated note with an attack shorter than the fade restarts without a click")
{
    // A released A4 is struck again a quarter cycle into its waveform. With a zero attack it
    // cannot rise from its releasing level without a step, so it restarts from a zero crossing
    // while the old voice fades out.
    constexpr double sampleRate = 48000.0;

    for (const double attack : { 0.0, 0.001 })
    {
        INFO("attack " << attack);
        const auto patch = makePatch(contracts::Waveform::sine, -12.0, attack, 0.0, 1.0, 0.3);
        const std::vector<Event> events { noteOnAt(0, 69, 127), noteOffAt(2400, 69), noteOnAt(atPeak, 69, 127) };

        Synth synth;
        synth.prepare(sampleRate, patch);
        const auto output = renderEvents(synth, events, atPeak + 1000);

        Synth reference;
        reference.prepare(sampleRate, patch);
        const auto steady = renderEvents(reference, { noteOnAt(0, 69, 127) }, 4800);
        const double fullStep = largestStep(steady, 2400, 4800);
        const double fadeAllowance = std::pow(10.0, -12.0 / 20.0) / fadeLength;

        CHECK(largestStep(output, atPeak - 10, atPeak + 300) <= 1.1 * fullStep + fadeAllowance);
        CHECK(peak(output, atPeak + 500, atPeak + 1000) > 0.2);
    }

    // An attack longer than the fade retriggers in place, from the current level.
    Synth synth;
    synth.prepare(sampleRate, makePatch(contracts::Waveform::sine, -12.0, 0.01, 0.0, 1.0, 0.3));
    renderEvents(synth, { noteOnAt(0, 69, 127), noteOffAt(2400, 69) }, atPeak);
    synth.noteOn(0, 69, 127);
    CHECK(synth.fadingVoiceCount() == 0);
    CHECK(soundingNote(synth, 69));
}

TEST_CASE("Resonance glides to its new value over 20 ms")
{
    // A4 at a 440 Hz cutoff: its level follows Q. Halfway through the glide Q is near the
    // geometric mean of 0.5 and 4; a jump would reach most of 4 within 10 ms.
    constexpr double sampleRate = 48000.0;
    const auto at = [](double q) { return makePatch(contracts::Waveform::sine, -24.0, 0.0, 0.0, 1.0, 0.5, 440.0, q); };

    Synth synth;
    synth.prepare(sampleRate, at(0.5));
    auto output = renderEvents(synth, { noteOnAt(0, 69, 127) }, atPeak);
    synth.setTargets(at(4.0));
    output.resize(atPeak + 4800);
    synth.render(output.data() + atPeak, 4800);

    const double final = unisonVoice * 4.0;
    CHECK(peak(output, atPeak + 400, atPeak + 520) < 0.6 * final);
    CHECK(std::abs(peak(output, atPeak + 3600, atPeak + 4800) / final - 1.0) < 0.02);
}

TEST_CASE("Preparing again at a new sample rate rescales every envelope time")
{
    const auto patch = makePatch(contracts::Waveform::sine, 0.0, 0.1, 0.1, 0.5, 0.2);
    const std::vector<Event> events { noteOnAt(0, 69, 100), noteOffAt(48000, 69) };

    Synth reprepared;
    reprepared.prepare(48000.0, patch);
    renderEvents(reprepared, events, 24000);
    reprepared.prepare(96000.0, patch);

    Synth fresh;
    fresh.prepare(96000.0, patch);

    CHECK_FALSE(firstBitDifference(renderEvents(reprepared, events, 96000), renderEvents(fresh, events, 96000)).has_value());
}

TEST_CASE("Parameter changes, steals and retriggers do not click")
{
    constexpr double sampleRate = 48000.0;
    const auto start = makePatch(contracts::Waveform::sine, -60.0);

    Synth reference;
    reference.prepare(sampleRate, makePatch(contracts::Waveform::sine, 0.0));
    const auto steady = renderEvents(reference, { noteOnAt(0, 69, 127) }, 9600);
    const double steadyStep = largestStep(steady, 4800, 9600);

    // Changes land a quarter cycle into the note's waveform, away from a zero crossing, and the
    // checks span the change itself.
    constexpr int changeAt = atPeak;

    SECTION("gain")
    {
        Synth synth;
        synth.prepare(sampleRate, start);
        auto output = renderEvents(synth, { noteOnAt(0, 69, 127) }, changeAt);
        synth.setTargets(makePatch(contracts::Waveform::sine, 0.0));
        output.resize(changeAt + 4800);
        synth.render(output.data() + changeAt, 4800);
        CHECK(largestStep(output, changeAt - 10, changeAt + 1000) <= 1.1 * steadyStep);
    }

    SECTION("sustain")
    {
        Synth synth;
        synth.prepare(sampleRate, makePatch(contracts::Waveform::sine, 0.0, 0.0, 0.0, 0.2));
        auto output = renderEvents(synth, { noteOnAt(0, 69, 127) }, changeAt);
        synth.setTargets(makePatch(contracts::Waveform::sine, 0.0));
        output.resize(changeAt + 4800);
        synth.render(output.data() + changeAt, 4800);
        CHECK(largestStep(output, changeAt - 10, changeAt + 1000) <= 1.1 * steadyStep);
        CHECK(peak(output, changeAt + 1000, changeAt + 4800) > 0.99 * peak(steady, 4800, 9600));
    }

    SECTION("cutoff")
    {
        Synth synth;
        synth.prepare(sampleRate, makePatch(contracts::Waveform::sine, 0.0, 0.0, 0.0, 1.0, 0.0, 200.0, 1.2));
        auto output = renderEvents(synth, { noteOnAt(0, 69, 127) }, changeAt);
        synth.setTargets(makePatch(contracts::Waveform::sine, 0.0, 0.0, 0.0, 1.0, 0.0, 20000.0, 1.2));
        output.resize(changeAt + 4800);
        synth.render(output.data() + changeAt, 4800);

        // Sweeping past the note raises its level by the filter's resonant peak, at most
        // Q / sqrt(1 - 1 / (4 Q^2)), about 1.32 at Q 1.2. A click would show as a spike in the
        // second difference far beyond that.
        const double resonantPeak = 1.2 / std::sqrt(1.0 - 1.0 / (4.0 * 1.2 * 1.2));
        CHECK(largestSecondDifference(output, changeAt - 10, changeAt + 1000)
              <= 1.5 * resonantPeak * largestSecondDifference(steady, 4800, 9600));
    }

    SECTION("steals and retriggers")
    {
        Synth synth;
        synth.prepare(sampleRate, makePatch(contracts::Waveform::sine, -24.0));
        std::vector<Event> events;

        for (int note = 50; note < 66; ++note)
            events.push_back(noteOnAt(0, note, 127));

        // Each lands a quarter cycle into the note it affects: note 50, stolen at 4800, and
        // note 55, retriggered at a lower velocity at 5449.
        events.push_back(noteOnAt(4800, 69, 127));
        events.push_back(noteOnAt(5449, 55, 30));
        const auto output = renderEvents(synth, events, 7200);

        Synth quiet;
        quiet.prepare(sampleRate, makePatch(contracts::Waveform::sine, -24.0));
        std::vector<Event> held(events.begin(), events.begin() + 16);
        const auto heldOnly = renderEvents(quiet, held, 7200);
        const double heldStep = largestStep(heldOnly, 2400, 4800);
        const double fadeAllowance = std::pow(10.0, -24.0 / 20.0) / std::round(declickSeconds * sampleRate);

        CHECK(largestStep(output, 4790, 4900) <= heldStep * 1.5 + fadeAllowance + 1.0e-6);
        CHECK(largestStep(output, 5440, 5550) <= heldStep * 1.5 + fadeAllowance + 1.0e-6);
    }
}

TEST_CASE("Every patch field reaches the sound of a playing note")
{
    constexpr double sampleRate = 48000.0;
    constexpr int changeAt = 4800;
    constexpr int total = 14400;

    const auto settledPeak = [&](const contracts::InstrumentPatch& first, const contracts::InstrumentPatch& second) {
        Synth synth;
        synth.prepare(sampleRate, first);
        auto output = renderEvents(synth, { noteOnAt(0, 69, 127) }, changeAt);
        synth.setTargets(second);
        output.resize(total);
        synth.render(output.data() + changeAt, total - changeAt);
        return peak(output, total - 2400, total);
    };

    const auto steadyPeak = [&](const contracts::InstrumentPatch& patch) {
        Synth synth;
        synth.prepare(sampleRate, patch);
        return peak(renderEvents(synth, { noteOnAt(0, 69, 127) }, total), total - 2400, total);
    };

    // A4 through the filter at its cutoff has gain Q exactly.
    const auto atCutoff = [](double q) {
        return makePatch(contracts::Waveform::sine, -24.0, 0.0, 0.0, 1.0, 0.5, 440.0, q);
    };

    CHECK(std::abs(steadyPeak(atCutoff(0.5)) / (unisonVoice * 0.5) - 1.0) < 0.01);
    CHECK(std::abs(steadyPeak(atCutoff(4.0)) / (unisonVoice * 4.0) - 1.0) < 0.01);

    const auto open = makePatch(contracts::Waveform::sine, -6.0, 0.0, 0.0, 1.0, 0.5);
    auto quieter = open;
    quieter.sustainLevel = 0.4;
    auto darker = open;
    darker.cutoffHz = 300.0;

    for (const auto& [name, first, second] : { std::tuple { "sustain", open, quieter },
                                               std::tuple { "cutoff", open, darker },
                                               std::tuple { "resonance", atCutoff(0.7071067811865476), atCutoff(4.0) } })
    {
        INFO(name);
        const double target = steadyPeak(second);
        CHECK(std::abs(steadyPeak(first) / target - 1.0) > 0.2);
        CHECK(std::abs(settledPeak(first, second) / target - 1.0) < 0.002);
    }

    SECTION("envelope times set after preparation shape the next note")
    {
        // Attack 0.1 s, then decay towards 0.5 at a rate that would take 0.2 s to fall from 1 to 0.
        Synth synth;
        synth.prepare(sampleRate, makePatch(contracts::Waveform::sine));
        synth.setTargets(makePatch(contracts::Waveform::sine, 0.0, 0.1, 0.2, 0.5, 0.5));
        const auto output = renderEvents(synth, { noteOnAt(0, 69, 127) }, 24000);

        const auto levelAt = [&](double seconds) {
            const auto centre = static_cast<std::size_t>(seconds * sampleRate);
            return peak(output, centre - 55, centre + 55);
        };

        constexpr double logRatio = 9.21044036697651; // ln((1 + 1e-4) / 1e-4)
        const double decayed = 0.5 - 1.0e-4 + (1.0 + 1.0e-4 - 0.5) * std::exp(-logRatio * 0.02 / 0.2);

        CHECK(std::abs(levelAt(0.05) - 0.5) < 0.02);
        CHECK(std::abs(levelAt(0.1) - 1.0) < 0.02);
        CHECK(std::abs(levelAt(0.12) - decayed) < 0.02);
        CHECK(std::abs(levelAt(0.45) - 0.5) < 0.005);
    }

    SECTION("a release time changed while a note plays applies to its release")
    {
        Synth synth;
        synth.prepare(sampleRate, makePatch(contracts::Waveform::sine, 0.0, 0.0, 0.0, 1.0, 0.5));
        auto output = renderEvents(synth, { noteOnAt(0, 69, 127) }, changeAt);
        synth.setTargets(makePatch(contracts::Waveform::sine, 0.0, 0.0, 0.0, 1.0, 0.05));
        synth.noteOff(0, 69);
        output.resize(changeAt + 4800);
        synth.render(output.data() + changeAt, 4800);

        CHECK(peak(output, changeAt + 1800, changeAt + 1900) > 1.0e-4);
        CHECK(peak(output, changeAt + 2402) == 0.0);
    }
}

TEST_CASE("All-notes-off releases a channel's notes and all-sound-off fades them out")
{
    constexpr int total = 4800;
    const auto patch = makePatch(contracts::Waveform::saw, 0.0, 0.0, 0.0, 1.0, 2.0);
    const std::vector<Event> playing { noteOnAt(0, 60, 100, 5), noteOnAt(0, 67, 100, 5), noteOnAt(0, 72, 100, 2) };

    SECTION("all-notes-off is a note-off for every held note on its channel")
    {
        auto released = playing;
        released.push_back(noteOffAt(37, 60, 5));
        released.push_back(noteOffAt(37, 67, 5));

        auto stopped = playing;
        stopped.push_back(controllerAt(37, 123, 0, 5));

        Synth reference;
        reference.prepare(48000.0, patch);
        Synth synth;
        synth.prepare(48000.0, patch);

        CHECK_FALSE(firstBitDifference(renderEvents(synth, stopped, total), renderEvents(reference, released, total)).has_value());
        CHECK(countVoices(synth, VoiceStage::release) == 2);
        CHECK(soundingNote(synth, 72, 2));
    }

    SECTION("all-sound-off fades every note on its channel out at once")
    {
        auto stopped = playing;
        stopped.push_back(controllerAt(37, 120, 0, 5));

        Synth reference;
        reference.prepare(48000.0, patch);
        const auto before = renderEvents(reference, playing, total);

        Synth other;
        other.prepare(48000.0, patch);
        const auto otherChannel = renderEvents(other, { noteOnAt(0, 72, 100, 2) }, total);

        Synth synth;
        synth.prepare(48000.0, patch);
        const auto output = renderEvents(synth, stopped, total);
        CHECK(synth.activeVoiceCount() == 1);

        for (std::size_t index = 0; index < 37; ++index)
            CHECK(std::bit_cast<std::uint32_t>(output[index]) == std::bit_cast<std::uint32_t>(before[index]));

        // Each of the two notes first drops by 1/144 of its level, not all of it; once the
        // fade ends only the other channel's note remains.
        const auto change = difference(output, before);
        CHECK(std::abs(change[37]) <= 2.0 * 1.2 * std::pow(100.0 / 127.0, 2.0) / fadeLength);

        for (auto index = static_cast<std::size_t>(37 + fadeLength); index < output.size(); ++index)
            CHECK(output[index] == otherChannel[index]);
    }
}

TEST_CASE("Stopping every voice returns the synthesiser to a freshly prepared state")
{
    Synth used;
    used.prepare(48000.0, busyPatch());
    renderEvents(used, standardScript(), 8000);
    used.stopAllVoicesNow();

    Synth fresh;
    fresh.prepare(48000.0, busyPatch());

    const auto a = renderEvents(used, standardScript(), 24000);
    const auto b = renderEvents(fresh, standardScript(), 24000);
    CHECK_FALSE(firstBitDifference(a, b).has_value());
}

TEST_CASE("Rendering is deterministic and independent of block sizes")
{
    const auto renderWith = [](const std::vector<int>& blockSizes) {
        Synth synth;
        synth.prepare(44100.0, busyPatch());
        return renderEvents(synth, standardScript(), 24000, blockSizes);
    };

    const auto whole = renderWith({ 24000 });
    CHECK(peak(whole) > 0.01);
    CHECK_FALSE(firstBitDifference(whole, renderWith({ 24000 })).has_value());

    for (const auto& sizes : std::vector<std::vector<int>> { { 1 }, { 17 }, { 128 }, { 441 }, { 0, 64, 0, 3 } })
        CHECK_FALSE(firstBitDifference(whole, renderWith(sizes)).has_value());

    std::mt19937 random(99u);
    std::uniform_int_distribution<int> size(0, 2048);
    std::vector<int> randomSizes;
    for (int index = 0; index < 64; ++index)
        randomSizes.push_back(size(random));
    randomSizes.push_back(64);

    CHECK_FALSE(firstBitDifference(whole, renderWith(randomSizes)).has_value());
}

TEST_CASE("Rendering does not depend on the processor's denormal mode")
{
    const auto renderWithMode = [](const contracts::InstrumentPatch& patch, unsigned int mode) {
        const auto saved = _mm_getcsr();
        _mm_setcsr(mode);
        Synth synth;
        synth.prepare(48000.0, patch);
        auto output = renderEvents(synth, standardScript(), 48000);
        _mm_setcsr(saved);
        return output;
    };

    // The second patch's sustain is so small that its output would fall in the float subnormal
    // range without the output flush.
    for (const double sustainLevel : { 0.001, 1.0e-36 })
    {
        INFO("sustain " << sustainLevel);
        const auto patch = makePatch(contracts::Waveform::saw, -40.0, 0.001, 0.05, sustainLevel, 0.3, 40.0, 0.2);
        const auto flushed = renderWithMode(patch, 0x9FC0);
        const auto precise = renderWithMode(patch, 0x1F80);
        CHECK_FALSE(firstBitDifference(flushed, precise).has_value());

        int subnormal = 0;
        for (const auto sample : precise)
            subnormal += std::fpclassify(sample) == FP_SUBNORMAL ? 1 : 0;

        CHECK(subnormal == 0);
    }
}

TEST_CASE("Level, velocity and pitch follow the patch")
{
    constexpr double sampleRate = 48000.0;
    const auto patch = makePatch(contracts::Waveform::sine, -12.0, 0.01, 0.1, 0.7, 0.2, 8000.0);

    const auto steadyPeak = [&](int velocity) {
        Synth synth;
        synth.prepare(sampleRate, patch);
        const auto output = renderEvents(synth, { noteOnAt(0, 69, velocity) }, 48000);
        return peak(output, 24000, 48000);
    };

    // Magnitude of the bilinear two-pole low-pass at 440 Hz, cutoff 8000 Hz, Q 1/sqrt(2).
    const double g = std::tan(std::numbers::pi * 8000.0 / sampleRate);
    const double k = 1.0 / 0.7071067811865476;
    const auto z = std::polar(1.0, 2.0 * std::numbers::pi * 440.0 / sampleRate);
    const auto s = (z - 1.0) / (z + 1.0) / g;
    const double filterGain = std::abs(1.0 / (s * s + k * s + 1.0));

    const double expected = std::pow(10.0, -12.0 / 20.0) * std::pow(100.0 / 127.0, 2.0) * 0.7 * filterGain;
    CHECK(std::abs(steadyPeak(100) / expected - 1.0) < 0.01);
    CHECK(std::abs(steadyPeak(64) / steadyPeak(127) - std::pow(64.0 / 127.0, 2.0)) < 0.005);

    for (const double rate : { 48000.0, 96000.0 })
    {
        Synth synth;
        synth.prepare(rate, patch);
        const auto output = renderEvents(synth, { noteOnAt(0, 69, 100) }, static_cast<int>(rate) * 2);
        int crossings = 0;

        for (std::size_t index = static_cast<std::size_t>(rate) + 1; index < output.size(); ++index)
            crossings += output[index - 1] < 0.0f && output[index] >= 0.0f ? 1 : 0;

        CHECK(std::abs(crossings - 440) <= 1);
    }
}

TEST_CASE("A note above the Nyquist frequency renders silence")
{
    Synth synth;
    synth.prepare(22050.0, makePatch(contracts::Waveform::saw));
    const auto output = renderEvents(synth, { noteOnAt(0, 127, 127) }, 2048);
    CHECK(allFinite(output));
    CHECK(peak(output) == 0.0);
}

TEST_CASE("MIDI parsing recognises notes and the stop controllers on every channel")
{
    Synth synth;
    synth.prepare(48000.0, makePatch(contracts::Waveform::sine, 0.0, 0.0, 0.0, 1.0, 1.0));

    for (int channel = 0; channel < 16; ++channel)
    {
        INFO("channel index " << channel);
        const int other = (channel + 1) % 16;
        const std::array<std::uint8_t, 3> on { static_cast<std::uint8_t>(0x90 | channel), 60, 100 };
        const std::array<std::uint8_t, 3> off { static_cast<std::uint8_t>(0x80 | channel), 60, 0 };
        const std::array<std::uint8_t, 3> allNotesOff { static_cast<std::uint8_t>(0xB0 | channel), 123, 0 };
        const std::array<std::uint8_t, 3> allSoundOff { static_cast<std::uint8_t>(0xB0 | channel), 120, 0 };

        synth.handleMidi(on.data(), 3);
        CHECK(synth.activeVoiceCount() == 1);
        synth.handleMidi(off.data(), 3);
        CHECK(countVoices(synth, VoiceStage::release) == 1);
        synth.stopAllVoicesNow();

        // Both controllers act on their own channel only.
        synth.noteOn(other, 61, 100);
        synth.handleMidi(on.data(), 3);
        synth.render(nullptr, 16);
        synth.handleMidi(allNotesOff.data(), 3);
        CHECK(countVoices(synth, VoiceStage::release) == 1);
        CHECK(soundingNote(synth, 61, other));

        synth.handleMidi(allSoundOff.data(), 3);
        CHECK(synth.fadingVoiceCount() == 1);
        CHECK(synth.activeVoiceCount() == 1);
        CHECK(soundingNote(synth, 61, other));
        synth.stopAllVoicesNow();
    }

    synth.noteOn(0, 60, 100);
    const std::array<std::uint8_t, 3> modulation { 0xB0, 1, 64 };
    const std::array<std::uint8_t, 3> pitchBend { 0xE0, 0, 64 };
    const std::array<std::uint8_t, 2> shortMessage { 0x90, 61 };
    std::array<std::uint8_t, 20> sysex {};
    sysex.front() = 0xF0;
    sysex.back() = 0xF7;

    synth.handleMidi(modulation.data(), 3);
    synth.handleMidi(pitchBend.data(), 3);
    synth.handleMidi(shortMessage.data(), 2);
    synth.handleMidi(sysex.data(), static_cast<int>(sysex.size()));
    synth.handleMidi(nullptr, 0);
    CHECK(synth.activeVoiceCount() == 1);
}
