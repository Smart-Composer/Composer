#include "Envelope.h"
#include "LinearSmoother.h"
#include "Spectrum.h"
#include "StateVariableFilter.h"
#include "SynthTestSupport.h"
#include "Wavetables.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <random>
#include <tuple>

namespace
{

using namespace composer;
using namespace composer::instrument::synth;
using namespace composer::instrument::synth::testing;

std::vector<double> tableCycle(contracts::Waveform waveform, int tableIndex)
{
    const float* table = WavetableBank::instance().table(waveform, tableIndex);
    return std::vector<double>(table, table + WavetableBank::tableSize);
}

int ticksUntil(EnvelopeState& envelope, const EnvelopeRates& rates, double sustain, VoiceStage stage, int limit)
{
    for (int tick = 1; tick <= limit; ++tick)
    {
        envelope.tick(rates, sustain);

        if (envelope.stage == stage)
            return tick;
    }

    return -1;
}

} // namespace

TEST_CASE("Every wavetable is band-limited to its harmonic limit")
{
    const auto check = [](contracts::Waveform waveform, int tableIndex, int highest) {
        const auto spectrum = magnitudeSpectrum(tableCycle(waveform, tableIndex));
        const double fundamental = spectrum[1];
        REQUIRE(fundamental > 1.0);

        double strongest = 0.0;

        for (std::size_t bin = static_cast<std::size_t>(highest) + 1; bin < spectrum.size(); ++bin)
            strongest = std::max(strongest, spectrum[bin]);

        CHECK(strongest < 1.0e-6 * fundamental);
    };

    check(contracts::Waveform::sine, 0, 1);

    for (int tableIndex = 0; tableIndex < static_cast<int>(WavetableBank::harmonicLimits.size()); ++tableIndex)
    {
        const int highest = WavetableBank::harmonicLimits[static_cast<std::size_t>(tableIndex)];
        INFO("table " << tableIndex << " with harmonics up to " << highest);
        check(contracts::Waveform::saw, tableIndex, highest);
        check(contracts::Waveform::square, tableIndex, highest);
    }
}

TEST_CASE("Each note plays the richest table that stays below the Nyquist frequency")
{
    const auto& limits = WavetableBank::harmonicLimits;

    for (const double sampleRate : { 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        for (int note = 0; note < 128; ++note)
        {
            const double fundamental = 440.0 * std::exp2((note - 69) / 12.0);
            const int selected = WavetableBank::selectTable(fundamental, sampleRate);
            INFO("note " << note << " at " << sampleRate);

            if (fundamental >= sampleRate / 2.0)
            {
                CHECK(selected == -1);
                continue;
            }

            REQUIRE(selected >= 0);
            CHECK(limits[static_cast<std::size_t>(selected)] * fundamental < sampleRate / 2.0);

            if (selected + 1 < static_cast<int>(limits.size()))
                CHECK(limits[static_cast<std::size_t>(selected + 1)] * fundamental >= sampleRate / 2.0);
        }
    }
}

TEST_CASE("Every waveform's table starts at an exact zero")
{
    const auto& bank = WavetableBank::instance();

    CHECK(bank.table(contracts::Waveform::sine, 0)[0] == 0.0f);

    for (int tableIndex = 0; tableIndex < static_cast<int>(WavetableBank::harmonicLimits.size()); ++tableIndex)
    {
        CHECK(bank.table(contracts::Waveform::square, tableIndex)[0] == 0.0f);
        CHECK(bank.table(contracts::Waveform::saw, tableIndex)[WavetableBank::tableSize / 2] == 0.0f);
    }
}

TEST_CASE("Rendered saw and square notes carry no audible aliasing")
{
    // Low notes play the richest tables, where interpolation error is largest; their harmonics
    // are close together, so they need a long analysis to leave bins between them.
    constexpr int settle = 2048;
    const std::vector<std::tuple<double, int, int>> cases {
        { 44100.0, 13, 1 << 18 }, { 44100.0, 15, 1 << 18 }, { 44100.0, 18, 1 << 18 }, { 44100.0, 24, 1 << 17 },
        { 44100.0, 60, 1 << 14 }, { 44100.0, 84, 1 << 14 }, { 44100.0, 96, 1 << 14 }, { 44100.0, 108, 1 << 14 },
        { 48000.0, 12, 1 << 18 }, { 48000.0, 18, 1 << 18 }, { 48000.0, 36, 1 << 16 }, { 48000.0, 100, 1 << 14 },
    };

    for (const auto waveform : { contracts::Waveform::saw, contracts::Waveform::square })
    {
        for (const auto& [sampleRate, note, analysed] : cases)
        {
            INFO("waveform " << static_cast<int>(waveform) << ", note " << note << " at " << sampleRate);
            Synth synth;
            synth.prepare(sampleRate, makePatch(waveform));
            const auto rendered = renderEvents(synth, { noteOnAt(0, note, 127) }, settle + analysed);

            std::vector<double> signal(rendered.begin() + settle, rendered.end());
            applyBlackmanHarris(signal);
            const auto spectrum = magnitudeSpectrum(signal);

            const double fundamentalHz = 440.0 * std::exp2((note - 69) / 12.0);
            const double binHz = sampleRate / analysed;
            const auto expectedBin = static_cast<std::size_t>(std::lround(fundamentalHz / binHz));

            std::size_t loudest = 1;
            for (std::size_t bin = 1; bin < spectrum.size(); ++bin)
                if (spectrum[bin] > spectrum[loudest])
                    loudest = bin;

            CHECK(loudest + 1 >= expectedBin);
            CHECK(loudest <= expectedBin + 1);

            const double limit = spectrum[loudest] * std::pow(10.0, -80.0 / 20.0);
            double worst = 0.0;

            for (std::size_t bin = 1; bin < spectrum.size(); ++bin)
            {
                const double frequency = static_cast<double>(bin) * binHz;
                const double harmonic = std::round(frequency / fundamentalHz);
                const double distanceInBins = std::abs(frequency - harmonic * fundamentalHz) / binHz;

                if (bin > 8 && distanceInBins > 8.0)
                    worst = std::max(worst, spectrum[bin]);
            }

            CHECK(worst < limit);
        }
    }
}

TEST_CASE("The low-pass filter has unity DC gain and gain Q at its cutoff")
{
    for (const double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto dc = SvfCoefficients::lowPass(1000.0, 0.7071067811865476, sampleRate);
        SvfState state;
        double output = 0.0;

        for (int index = 0; index < static_cast<int>(2.0 * sampleRate); ++index)
            output = state.processLowPass(1.0, dc);

        CHECK(std::abs(output - 1.0) < 1.0e-6);

        for (const double q : { 0.1, 0.7071067811865476, 10.0 })
        {
            INFO("rate " << sampleRate << ", Q " << q);
            constexpr double cutoff = 1000.0;
            const auto coefficients = SvfCoefficients::lowPass(cutoff, q, sampleRate);
            SvfState sine;
            double amplitude = 0.0;
            const int total = static_cast<int>(3.0 * sampleRate);

            for (int index = 0; index < total; ++index)
            {
                const double input = std::sin(2.0 * std::numbers::pi * cutoff * index / sampleRate);
                const double y = sine.processLowPass(input, coefficients);

                if (index > total - static_cast<int>(sampleRate / 2.0))
                    amplitude = std::max(amplitude, std::abs(y));
            }

            CHECK(std::abs(amplitude / q - 1.0) < 0.01);
        }
    }
}

TEST_CASE("The filter stays bounded under per-sample coefficient changes")
{
    constexpr double sampleRate = 48000.0;
    std::mt19937 random(7u);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    SvfState state;
    double largest = 0.0;

    for (int index = 0; index < 1000000; ++index)
    {
        const double cutoff = 20.0 * std::pow(0.49 * sampleRate / 20.0, unit(random));
        const double q = 0.1 * std::pow(100.0, unit(random));
        const double input = 2.0 * unit(random) - 1.0;
        const double y = state.processLowPass(input, SvfCoefficients::lowPass(cutoff, q, sampleRate));
        REQUIRE(std::isfinite(y));
        largest = std::max(largest, std::abs(y));
    }

    for (int index = 0; index < 100000; ++index)
    {
        const bool high = (index / 7) % 2 == 0;
        const double y = state.processLowPass(index % 2 == 0 ? 1.0 : -1.0,
                                              SvfCoefficients::lowPass(high ? 0.49 * sampleRate : 20.0, 10.0, sampleRate));
        REQUIRE(std::isfinite(y));
        largest = std::max(largest, std::abs(y));
    }

    CHECK(largest < 64.0);
}

TEST_CASE("The effective cutoff never reaches the Nyquist frequency")
{
    for (const double sampleRate : { 22050.0, 44100.0, 48000.0, 96000.0 })
    {
        Synth synth;
        synth.prepare(sampleRate, makePatch(contracts::Waveform::sine, 0.0, 0.0, 0.0, 1.0, 0.0, 20000.0));
        CHECK(synth.effectiveCutoffHz() < sampleRate / 2.0);
        CHECK(synth.effectiveCutoffHz() <= 20000.0);

        synth.prepare(sampleRate, makePatch(contracts::Waveform::sine, 0.0, 0.0, 0.0, 1.0, 0.0, 5000.0));
        CHECK(std::abs(synth.effectiveCutoffHz() - 5000.0) < 1.0e-9);
    }
}

TEST_CASE("Envelope stages take their stated times")
{
    constexpr double sampleRate = 48000.0;
    constexpr double logRatio = 9.21044036697651; // ln((1 + 1e-4) / 1e-4)

    SECTION("attack reaches full level in the attack time")
    {
        EnvelopeRates rates;
        rates.set(0.01, 0.1, 0.2, sampleRate);
        EnvelopeState envelope { VoiceStage::attack, 0.0 };
        const int ticks = ticksUntil(envelope, rates, 0.5, VoiceStage::decay, 10000);
        CHECK(std::abs(ticks - 480) <= 1);
        CHECK(envelope.level == 1.0);
    }

    SECTION("decay reaches the sustain level at the predicted time")
    {
        EnvelopeRates rates;
        rates.set(0.0, 0.1, 0.2, sampleRate);
        EnvelopeState envelope { VoiceStage::decay, 1.0 };
        constexpr double sustain = 0.5;
        const double expected = 0.1 * sampleRate * std::log((1.0 - sustain + 1.0e-4) / 1.0e-4) / logRatio;
        const int ticks = ticksUntil(envelope, rates, sustain, VoiceStage::sustain, 100000);
        CHECK(std::abs(ticks - expected) <= 2.0);
        CHECK(envelope.level == sustain);
    }

    SECTION("release ends exactly at zero in the release time")
    {
        EnvelopeRates rates;
        rates.set(0.0, 0.0, 0.2, sampleRate);
        EnvelopeState envelope { VoiceStage::release, 1.0 };
        const int ticks = ticksUntil(envelope, rates, 1.0, VoiceStage::idle, 100000);
        CHECK(std::abs(ticks - std::ceil(0.2 * sampleRate)) <= 1.0);
        CHECK(envelope.level == 0.0);

        EnvelopeState partial { VoiceStage::release, 0.7 };
        const double expected = 0.2 * sampleRate * std::log((0.7 + 1.0e-4) / 1.0e-4) / logRatio;
        CHECK(std::abs(ticksUntil(partial, rates, 1.0, VoiceStage::idle, 100000) - expected) <= 2.0);
    }

    SECTION("zero-length stages are immediate")
    {
        EnvelopeRates rates;
        rates.set(0.0, 0.0, 0.0, sampleRate);
        EnvelopeState envelope { VoiceStage::attack, 0.0 };
        CHECK(envelope.tick(rates, 0.6) == 0.6);
        CHECK(envelope.stage == VoiceStage::sustain);

        envelope.stage = VoiceStage::release;
        CHECK(envelope.tick(rates, 0.6) == 0.0);
        CHECK(envelope.stage == VoiceStage::idle);
    }

    SECTION("extreme times and rates never produce NaN")
    {
        for (const double rate : { 8000.0, 44100.0, 192000.0, 768000.0 })
        {
            for (const double time : { 0.0, 1.0e-9, 1.0 / rate, 10.0 })
            {
                EnvelopeRates rates;
                rates.set(time, time, time, rate);
                EnvelopeState envelope { VoiceStage::attack, 0.0 };

                for (int tick = 0; tick < 2000; ++tick)
                {
                    if (tick == 1000)
                        envelope.stage = VoiceStage::release;

                    REQUIRE(std::isfinite(envelope.tick(rates, 0.5)));
                }
            }
        }
    }

    SECTION("a retrigger attacks from the current level without a jump")
    {
        EnvelopeRates rates;
        rates.set(0.01, 0.1, 0.2, sampleRate);
        EnvelopeState envelope { VoiceStage::release, 0.4 };
        double previous = envelope.level;
        envelope.stage = VoiceStage::attack;

        for (int tick = 0; tick < 1000; ++tick)
        {
            const double level = envelope.tick(rates, 0.5);
            CHECK(std::abs(level - previous) <= rates.attackIncrement + 1.0e-12);
            previous = level;
        }
    }
}

TEST_CASE("Parameter ramps land exactly on their target")
{
    LinearSmoother smoother;
    smoother.setLength(960);
    smoother.setTarget(-12.0);
    smoother.snap();

    smoother.setTarget(0.0);
    int samples = 0;

    while (smoother.isRamping())
    {
        smoother.next();
        ++samples;
    }

    CHECK(samples == 960);
    CHECK(std::bit_cast<std::uint64_t>(smoother.value()) == std::bit_cast<std::uint64_t>(0.0));

    const double tiny = std::nextafter(0.0, 1.0);
    smoother.setTarget(tiny);
    CHECK(smoother.isRamping());

    smoother.snap();
    smoother.setTarget(tiny);
    CHECK_FALSE(smoother.isRamping());

    smoother.setTarget(1.0);
    for (int index = 0; index < 480; ++index)
        smoother.next();

    const double midway = smoother.value();
    smoother.setTarget(-1.0);
    CHECK(std::abs(smoother.next() - midway) < 2.0 / 960.0);
}
