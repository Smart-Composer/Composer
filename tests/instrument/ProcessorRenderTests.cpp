#include "AllocationProbe.h"
#include "PatchFixtures.h"
#include "RenderHarness.h"

#include <composer/instrument/InstrumentProcessor.h>
#include <composer/instrument/synth/ParameterCurves.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <limits>
#include <random>
#include <thread>

namespace
{

using namespace composer;
using namespace composer::tests;
using composer::instrument::InstrumentProcessor;
namespace synth = composer::instrument::synth;

contracts::InstrumentPatch plainPatch(contracts::Waveform waveform = contracts::Waveform::sine)
{
    contracts::InstrumentPatch patch;
    patch.waveform = waveform;
    patch.gainDb = -6.0;
    patch.attackSeconds = 0.0;
    patch.decaySeconds = 0.0;
    patch.sustainLevel = 1.0;
    patch.releaseSeconds = 0.2;
    patch.cutoffHz = 12000.0;
    patch.resonanceQ = 0.7071067811865476;
    return patch;
}

void prepare(InstrumentProcessor& processor, double sampleRate, int blockSize, const contracts::InstrumentPatch& patch)
{
    REQUIRE_FALSE(processor.applyPatch(patch).has_value());
    processor.setRateAndBufferSizeDetails(sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);
}

} // namespace

TEST_CASE("An applied patch plays from the next processed block")
{
    constexpr int block = 128;
    constexpr int changeBlock = 40;
    const MidiScript script { noteOn(0, 64, 100) };
    auto louder = plainPatch();
    louder.gainDb = 0.0;

    InstrumentProcessor steady;
    InstrumentProcessor changed;
    InstrumentProcessor target;
    prepare(steady, 48000.0, block, plainPatch());
    prepare(changed, 48000.0, block, plainPatch());
    prepare(target, 48000.0, block, louder);

    const auto a = renderScript(steady, script, 48000, block);
    const auto b = renderScript(changed, script, 48000, block, [&](int index) {
        if (index == changeBlock)
            REQUIRE_FALSE(changed.applyPatch(louder).has_value());
    });
    const auto c = renderScript(target, script, 48000, block);

    const int changeSample = changeBlock * block;
    const auto firstDifference = firstBitDifference(a, b);
    REQUIRE(firstDifference.has_value());
    CHECK(*firstDifference >= changeSample);
    CHECK(*firstDifference < changeSample + 8);

    const int settled = changeSample + static_cast<int>(std::lround(0.020 * 48000.0));
    bool equalAfterRamp = true;

    for (int channel = 0; channel < 2; ++channel)
        for (int index = settled; index < 48000; ++index)
            equalAfterRamp = equalAfterRamp && b.getSample(channel, index) == c.getSample(channel, index);

    CHECK(equalAfterRamp);
}

TEST_CASE("A waveform change applies only to notes started afterwards")
{
    constexpr int block = 64;
    const MidiScript held { noteOn(0, 60, 100) };
    const MidiScript both { noteOn(0, 60, 100), noteOn(9600, 67, 100) };

    InstrumentProcessor sineOnly;
    prepare(sineOnly, 48000.0, block, plainPatch());
    const auto reference = renderScript(sineOnly, held, 19200, block);

    InstrumentProcessor switching;
    prepare(switching, 48000.0, block, plainPatch());
    const auto output = renderScript(switching, both, 19200, block, [&](int index) {
        if (index == 100)
            REQUIRE_FALSE(switching.applyPatch(plainPatch(contracts::Waveform::square)).has_value());
    });

    bool heldUnchanged = true;
    for (int index = 0; index < 9600; ++index)
        heldUnchanged = heldUnchanged && output.getSample(0, index) == reference.getSample(0, index);
    CHECK(heldUnchanged);

    InstrumentProcessor squareReference;
    prepare(squareReference, 48000.0, block, plainPatch(contracts::Waveform::square));
    const auto expectedLater = renderScript(squareReference, { noteOn(0, 67, 100) }, 9600, block);

    double largestDifference = 0.0;
    for (int index = 9600; index < 19200; ++index)
    {
        const double expected = reference.getSample(0, index) + expectedLater.getSample(0, index - 9600);
        largestDifference = std::max(largestDifference, std::abs(expected - output.getSample(0, index)));
    }

    CHECK(largestDifference < 1.0e-5);
}

TEST_CASE("A released note decays to exact silence in its release time")
{
    constexpr double rate = 48000.0;
    constexpr int noteOffAt = 24000;

    InstrumentProcessor processor;
    prepare(processor, rate, 128, plainPatch());
    const auto output = renderScript(processor, { noteOn(0, 69, 127), noteOff(noteOffAt, 69) }, 48000, 128);

    double rms = 0.0;
    for (int index = noteOffAt + 9000; index < noteOffAt + 9400; ++index)
        rms += output.getSample(0, index) * output.getSample(0, index);
    CHECK(rms > 0.0);

    CHECK(silentFrom(output, noteOffAt + static_cast<int>(std::ceil(0.2 * rate)) + 2));

    double previousWindow = std::numeric_limits<double>::infinity();
    for (int window = 0; window < 18; ++window)
    {
        double energy = 0.0;
        for (int index = 0; index < 480; ++index)
        {
            const float sample = output.getSample(0, noteOffAt + window * 480 + index);
            energy += sample * sample;
        }

        CHECK(energy < previousWindow);
        previousWindow = energy;
    }

    auto immediate = plainPatch();
    immediate.releaseSeconds = 0.0;
    InstrumentProcessor cut;
    prepare(cut, rate, 128, immediate);
    const auto cutOutput = renderScript(cut, { noteOn(0, 69, 127), noteOff(noteOffAt, 69) }, 48000, 128);
    CHECK(silentFrom(cutOutput, noteOffAt));
}

TEST_CASE("Panic fades every voice out at the next block from any thread, and a host reset cuts it")
{
    static_assert(noexcept(std::declval<InstrumentProcessor&>().panic()));

    auto longRelease = plainPatch();
    longRelease.releaseSeconds = 10.0;

    MidiScript chord;
    for (int note = 48; note < 64; ++note)
        chord.push_back(noteOn(0, note, 100));

    constexpr int stopAt = 1280;
    constexpr int fade = 144;

    const auto check = [&](const std::function<void(InstrumentProcessor&)>& stop, bool fades) {
        InstrumentProcessor processor;
        prepare(processor, 48000.0, 128, longRelease);
        MidiScript held = chord;
        for (int note = 48; note < 64; ++note)
            held.push_back(noteOff(stopAt, note));

        const auto output = renderScript(processor, held, 128 * 30, 128, [&](int index) {
            if (index == stopAt / 128)
                stop(processor);
        });

        CHECK(peak(output, 0, stopAt) > 0.01f);
        CHECK(silentFrom(output, stopAt + (fades ? fade : 0)));
        CHECK(processor.currentPatch() == longRelease);

        if (fades)
        {
            // The fade's first step stays near the chord's own movement; a cut would drop the
            // whole chord in one sample.
            double before = 0.0;
            for (int index = stopAt - 100; index < stopAt; ++index)
                before = std::max(before, std::abs(static_cast<double>(output.getSample(0, index)) - output.getSample(0, index - 1)));

            const double step = std::abs(static_cast<double>(output.getSample(0, stopAt)) - output.getSample(0, stopAt - 1));
            CHECK(peak(output, stopAt, stopAt + 10) > 0.01f);
            CHECK(step <= 1.1 * before + 16.0 * 0.5 / fade);
        }

        InstrumentProcessor fresh;
        prepare(fresh, 48000.0, 128, longRelease);
        const MidiScript later { noteOn(0, 72, 100) };
        CHECK_FALSE(firstBitDifference(renderScript(processor, later, 4800, 128),
                                       renderScript(fresh, later, 4800, 128)).has_value());
    };

    check([](InstrumentProcessor& processor) {
        std::thread other([&processor] { processor.panic(); });
        other.join();
    }, true);
    check([](InstrumentProcessor& processor) { processor.panic(); }, true);
    check([](InstrumentProcessor& processor) { processor.reset(); }, false);
}

TEST_CASE("All-notes-off releases and all-sound-off fades out inside a block")
{
    auto patch = plainPatch();
    patch.releaseSeconds = 5.0;

    InstrumentProcessor released;
    prepare(released, 48000.0, 512, patch);
    const auto expected = renderScript(released, { noteOn(0, 60, 100), noteOff(37, 60), noteOn(400, 64, 100) }, 1024, 512);

    InstrumentProcessor allNotesOff;
    prepare(allNotesOff, 48000.0, 512, patch);
    const auto releasing = renderScript(allNotesOff, { noteOn(0, 60, 100), controller(37, 123, 0), noteOn(400, 64, 100) }, 1024, 512);
    CHECK_FALSE(firstBitDifference(releasing, expected).has_value());
    CHECK(peak(releasing, 37, 400) > 0.01f);

    InstrumentProcessor allSoundOff;
    prepare(allSoundOff, 48000.0, 512, patch);
    const auto fading = renderScript(allSoundOff, { noteOn(0, 60, 100), controller(37, 120, 0), noteOn(400, 64, 100) }, 1024, 512);

    bool before = true;
    for (int index = 0; index < 37; ++index)
        before = before && fading.getSample(0, index) == expected.getSample(0, index);

    CHECK(before);
    CHECK(peak(fading, 37, 37 + 144) > 0.0f);
    CHECK(silentFrom(fading, 37 + 144, 400));
    CHECK(peak(fading, 400) > 0.01f);
}

TEST_CASE("Bypass fades the instrument out and leaves no note hanging")
{
    InstrumentProcessor processor;
    prepare(processor, 48000.0, 256, plainPatch());

    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    const std::array<std::uint8_t, 3> on { 0x90, 60, 100 };
    const std::array<std::uint8_t, 3> off { 0x80, 60, 0 };

    midi.addEvent(on.data(), 3, 0);
    processor.processBlock(buffer, midi);
    CHECK(peak(buffer) > 0.01f);
    const float last = buffer.getSample(0, 255);

    // MIDI is ignored while bypassed: the note-off never arrives, a new note never starts, and
    // the held note fades out over 3 ms from where it was.
    const std::array<std::uint8_t, 3> another { 0x90, 64, 100 };
    midi.clear();
    midi.addEvent(off.data(), 3, 10);
    midi.addEvent(another.data(), 3, 10);
    buffer.applyGain(std::numeric_limits<float>::quiet_NaN());
    processor.processBlockBypassed(buffer, midi);
    CHECK(allFinite(buffer));
    CHECK(std::abs(buffer.getSample(0, 0) - last) < 0.05f);
    CHECK(peak(buffer, 0, 10) > 0.01f);
    CHECK(silentFrom(buffer, 144));
    CHECK(midi.isEmpty());

    midi.clear();
    processor.processBlock(buffer, midi);
    CHECK(silentFrom(buffer, 0));

    midi.addEvent(on.data(), 3, 0);
    processor.processBlock(buffer, midi);
    CHECK(peak(buffer) > 0.01f);
}

TEST_CASE("Patches at their bounds render finite, bounded output under automation")
{
    for (const auto& [name, patch] : loadPatchFixtures())
    {
        for (const double rate : { 44100.0, 48000.0, 96000.0 })
        {
            INFO(name << " at " << rate);
            InstrumentProcessor processor;
            prepare(processor, rate, 512, patch);

            MidiScript chord;
            for (int note = 40; note < 56; ++note)
                chord.push_back(noteOn(0, note, 127));

            const int total = static_cast<int>(rate);
            const int blocks = total / 512;
            auto* cutoff = processor.getParameters()[6];

            const auto output = renderScript(processor, chord, total, 512, [&](int index) {
                cutoff->setValue(static_cast<float>(index) / static_cast<float>(std::max(1, blocks - 1)));
            });

            CHECK(allFinite(output));
            CHECK(peak(output) < 64.0f);

            if (name == "lower_bounds")
                CHECK(silentFrom(output, 0));
        }
    }
}

TEST_CASE("Processor output does not depend on block sizes")
{
    const auto patch = loadPatchFixtures().back().second;
    const auto script = standardScript(48000.0);

    const auto renderWith = [&](const std::vector<int>& sizes) {
        InstrumentProcessor processor;
        prepare(processor, 48000.0, 4096, patch);
        juce::AudioBuffer<float> output(2, 120000);
        juce::AudioBuffer<float> block(2, 4096);
        juce::MidiBuffer midi;
        std::size_t nextEvent = 0;
        std::size_t nextSize = 0;

        for (int start = 0; start < output.getNumSamples();)
        {
            const int length = std::min(sizes[nextSize++ % sizes.size()], output.getNumSamples() - start);
            block.setSize(2, length, false, false, true);
            midi.clear();

            while (nextEvent < script.size() && script[nextEvent].sampleTime < start + length)
            {
                midi.addEvent(script[nextEvent].bytes.data(), 3, script[nextEvent].sampleTime - start);
                ++nextEvent;
            }

            processor.processBlock(block, midi);

            for (int channel = 0; channel < 2; ++channel)
                output.copyFrom(channel, start, block, channel, 0, length);

            start += length;
        }

        return output;
    };

    const auto reference = renderWith({ 4096 });
    CHECK(peak(reference) > 0.01f);

    for (const auto& sizes : std::vector<std::vector<int>> { { 1 }, { 7 }, { 128 }, { 0, 128, 0, 7 } })
        CHECK_FALSE(firstBitDifference(reference, renderWith(sizes)).has_value());
}

TEST_CASE("Processing never allocates or frees memory")
{
    if (! AllocationProbe::available())
        SKIP("Allocation counting needs the debug C runtime.");

    InstrumentProcessor processor;
    prepare(processor, 48000.0, 256, loadPatchFixtures().back().second);

    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    midi.ensureSize(4096);
    std::array<std::uint8_t, 20> sysex {};
    sysex.front() = 0xF0;
    sysex.back() = 0xF7;
    long allocations = 0;
    std::mt19937 random(11u);

    for (int block = 0; block < 500; ++block)
    {
        midi.clear();

        for (int index = 0; index < 4; ++index)
        {
            const auto note = static_cast<std::uint8_t>(36 + (random() % 48));
            const std::array<std::uint8_t, 3> on { 0x90, note, 100 };
            const std::array<std::uint8_t, 3> off { 0x80, static_cast<std::uint8_t>(36 + (random() % 48)), 0 };
            midi.addEvent(on.data(), 3, static_cast<int>(random() % 256));
            midi.addEvent(off.data(), 3, static_cast<int>(random() % 256));
        }

        if (block % 50 == 0)
        {
            const std::array<std::uint8_t, 3> stop { 0xB0, static_cast<std::uint8_t>(block % 100 == 0 ? 123 : 120), 0 };
            midi.addEvent(stop.data(), 3, 128);
            midi.addEvent(sysex.data(), static_cast<int>(sysex.size()), 64);
        }

        AllocationProbe::arm();
        processor.processBlock(buffer, midi);
        allocations += AllocationProbe::disarm();

        // Between blocks, unmeasured: host automation, patch changes, panic and re-preparation.
        processor.getParameters()[static_cast<int>(random() % 8)]->setValue(static_cast<float>(random() % 1000) / 999.0f);

        if (block % 37 == 0)
            REQUIRE_FALSE(processor.applyPatch(awkwardPatch()).has_value());

        if (block % 41 == 0)
            processor.panic();

        if (block % 167 == 0)
        {
            const double rate = block % 2 == 0 ? 44100.0 : 96000.0;
            processor.prepareToPlay(rate, 256);
        }
    }

    CHECK(allocations == 0);
}

TEST_CASE("Concurrent patch changes, automation, panic and restores keep output sound")
{
    InstrumentProcessor processor;
    prepare(processor, 48000.0, 64, plainPatch());
    const auto fixtures = loadPatchFixtures();
    std::atomic<bool> running { true };
    std::atomic<bool> outputSound { true };
    std::atomic<float> loudest { 0.0f };

    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(2, 64);
        juce::MidiBuffer midi;
        std::mt19937 random(1u);

        for (int block = 0; block < 2000; ++block)
        {
            midi.clear();
            const std::array<std::uint8_t, 3> on { 0x90, static_cast<std::uint8_t>(40 + (random() % 40)), 110 };
            midi.addEvent(on.data(), 3, 0);
            processor.getParameters()[static_cast<int>(random() % 8)]->setValue(static_cast<float>(random() % 1000) / 999.0f);
            processor.processBlock(buffer, midi);

            if (! allFinite(buffer))
                outputSound = false;

            loudest = std::max(loudest.load(), peak(buffer));
        }

        running = false;
    });

    std::thread panicking([&] {
        while (running)
            processor.panic();
    });

    std::thread restoring([&] {
        std::size_t index = 0;

        while (running)
        {
            const auto bytes = std::get<std::string>(contracts::encodePatch(fixtures[index++ % fixtures.size()].second));
            processor.setStateInformation(bytes.data(), static_cast<int>(bytes.size()));
        }
    });

    for (int index = 0; index < 2000 && running; ++index)
        REQUIRE_FALSE(processor.applyPatch(index % 2 == 0 ? awkwardPatch() : plainPatch()).has_value());

    audio.join();
    panicking.join();
    restoring.join();

    CHECK(outputSound);
    CHECK(loudest < 64.0f);

    REQUIRE_FALSE(processor.applyPatch(awkwardPatch()).has_value());
    CHECK(processor.currentPatch() == awkwardPatch());

    for (std::size_t index = 0; index < synth::parameterCount; ++index)
        CHECK(processor.getParameters()[static_cast<int>(index)]->getValue()
              == synth::normalise(index, synth::fieldValue(awkwardPatch(), index)));
}

TEST_CASE("The instrument reports a tail long enough for its longest release")
{
    InstrumentProcessor processor;
    CHECK(processor.getTailLengthSeconds() == 10.003);
}
