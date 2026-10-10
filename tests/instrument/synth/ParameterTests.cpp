#include "SynthTestSupport.h"

#include <composer/instrument/synth/ParameterCurves.h>
#include <composer/instrument/synth/PatchState.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <random>
#include <thread>

namespace
{

using namespace composer;
using namespace composer::instrument::synth;

std::size_t indexOf(std::string_view id)
{
    for (std::size_t index = 0; index < parameterCount; ++index)
        if (contracts::parameterDescriptors[index].id == id)
            return index;

    FAIL_CHECK("unknown parameter " << id);
    return 0;
}

bool isChoice(std::size_t index)
{
    return contracts::parameterDescriptors[index].kind == contracts::ParameterKind::choice;
}

} // namespace

TEST_CASE("Every patch parameter has a curve, in descriptor order")
{
    REQUIRE(parameterCurves.size() == contracts::parameterDescriptors.size());

    for (std::size_t index = 0; index < parameterCount; ++index)
        CHECK(parameterCurves[index].id == contracts::parameterDescriptors[index].id);
}

TEST_CASE("Curves map the ends of the host range exactly onto the parameter bounds")
{
    for (std::size_t index = 0; index < parameterCount; ++index)
    {
        const auto& descriptor = contracts::parameterDescriptors[index];
        INFO(descriptor.id);

        CHECK(std::bit_cast<std::uint64_t>(denormalise(index, 0.0f)) == std::bit_cast<std::uint64_t>(descriptor.minimum));
        CHECK(std::bit_cast<std::uint64_t>(denormalise(index, 1.0f)) == std::bit_cast<std::uint64_t>(descriptor.maximum));
        CHECK(normalise(index, descriptor.minimum) == 0.0f);
        CHECK(normalise(index, descriptor.maximum) == 1.0f);
        CHECK(denormalise(index, -1.0f) == descriptor.minimum);
        CHECK(denormalise(index, 2.0f) == descriptor.maximum);

        double previous = -std::numeric_limits<double>::infinity();
        constexpr int steps = 65536;

        for (int step = 0; step <= steps; ++step)
        {
            const auto n = static_cast<float>(step) / static_cast<float>(steps);
            const double value = denormalise(index, n);

            REQUIRE(value >= descriptor.minimum);
            REQUIRE(value <= descriptor.maximum);

            if (isChoice(index))
                REQUIRE(value >= previous);
            else
                REQUIRE(value > previous);

            previous = value;

            contracts::InstrumentPatch patch;
            setFieldValue(patch, index, value);
            REQUIRE_FALSE(contracts::validatePatch(patch).has_value());
        }
    }
}

TEST_CASE("Host values survive a round trip through the parameter value")
{
    std::mt19937 random(20261009u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    for (std::size_t index = 0; index < parameterCount; ++index)
    {
        INFO(contracts::parameterDescriptors[index].id);

        if (isChoice(index))
        {
            for (const float n : { 0.0f, 0.5f, 1.0f })
                CHECK(normalise(index, denormalise(index, n)) == n);

            continue;
        }

        const auto check = [index](float n) {
            const float back = normalise(index, denormalise(index, n));

            if (back != n)
                FAIL_CHECK("normalised " << n << " came back as " << back);
        };

        check(0.0f);
        check(1.0f);

        for (int k = 0; k <= 4096; ++k)
            check(static_cast<float>(k) / 4096.0f);

        for (int k = 0; k <= (1 << 18); ++k)
            check(std::ldexp(1.0f, -20) + (1.0f - std::ldexp(1.0f, -20)) * static_cast<float>(k) / static_cast<float>(1 << 18));

        for (int k = 0; k < 65536; ++k)
        {
            const float n = unit(random);

            if (n >= std::ldexp(1.0f, -20))
                check(n);
        }
    }
}

TEST_CASE("The normalised defaults and curve midpoints are fixed")
{
    CHECK(normalise(indexOf("gain_db"), -12.0) == static_cast<float>(0.8));
    CHECK(normalise(indexOf("attack_seconds"), 0.01) == static_cast<float>(0.10000000000000002));
    CHECK(normalise(indexOf("decay_seconds"), 0.1) == static_cast<float>(0.2154434690031884));
    CHECK(normalise(indexOf("sustain_level"), 0.7) == static_cast<float>(0.7));
    CHECK(normalise(indexOf("release_seconds"), 0.2) == static_cast<float>(0.2714417616594907));
    CHECK(normalise(indexOf("cutoff_hz"), 8000.0) == static_cast<float>(0.8673533304426542));
    CHECK(normalise(indexOf("resonance_q"), 0.7071067811865476) == static_cast<float>(0.42474250108400463));
    CHECK(normalise(indexOf("waveform"), 1.0) == 0.5f);

    CHECK(denormalise(indexOf("attack_seconds"), 0.5f) == 1.25);
    CHECK(std::abs(denormalise(indexOf("cutoff_hz"), 0.5f) - 632.4555320336758) < 1.0e-9);
    CHECK(std::abs(denormalise(indexOf("resonance_q"), 0.5f) - 1.0) < 1.0e-12);
}

TEST_CASE("The patch state holds a published patch exactly and filters host echoes")
{
    auto patch = testing::makePatch(contracts::Waveform::square, -0.0, 5.0e-324, 0.123456789, 0.3,
                                    9.87654321, 1234.5678901, 3.14159);
    PatchState state;

    const auto changed = state.publish(patch);
    const auto snapshot = state.snapshot();

    contracts::InstrumentPatch read;
    REQUIRE(state.tryRead(read));

    for (std::size_t index = 0; index < parameterCount; ++index)
    {
        INFO(contracts::parameterDescriptors[index].id);
        const auto expected = std::bit_cast<std::uint64_t>(fieldValue(patch, index));
        CHECK(std::bit_cast<std::uint64_t>(fieldValue(snapshot, index)) == expected);
        CHECK(std::bit_cast<std::uint64_t>(fieldValue(read, index)) == expected);
        CHECK(changed.test(index) == (normalise(index, fieldValue(contracts::InstrumentPatch {}, index))
                                      != normalise(index, fieldValue(patch, index))));

        // Echoing the value the host was shown changes nothing.
        state.setHostValue(index, state.hostValue(index));
        CHECK(std::bit_cast<std::uint64_t>(fieldValue(state.snapshot(), index)) == expected);
    }

    CHECK(state.publish(patch).none());

    const auto cutoff = indexOf("cutoff_hz");
    state.setHostValue(cutoff, 0.25f);
    const auto edited = state.snapshot();
    CHECK(edited.cutoffHz == denormalise(cutoff, 0.25f));
    CHECK(state.hostValue(cutoff) == 0.25f);
    CHECK(std::bit_cast<std::uint64_t>(edited.decaySeconds) == std::bit_cast<std::uint64_t>(patch.decaySeconds));
    CHECK_FALSE(contracts::validatePatch(edited).has_value());

    state.setHostValue(cutoff, std::numeric_limits<float>::quiet_NaN());
    CHECK(state.snapshot().cutoffHz == edited.cutoffHz);

    state.setHostValue(cutoff, -1.0f);
    CHECK(state.snapshot().cutoffHz == 20.0);
    state.setHostValue(cutoff, 2.0f);
    CHECK(state.snapshot().cutoffHz == 20000.0);

    const auto waveform = indexOf("waveform");
    state.setHostValue(waveform, 0.3f);
    CHECK(state.snapshot().waveform == contracts::Waveform::saw);
    CHECK(state.hostValue(waveform) == 0.5f);
}

TEST_CASE("The audio-thread read never sees a mixture of two published patches")
{
    const auto patchFor = [](int k) {
        const double m = static_cast<double>(k % 1000);
        auto patch = testing::makePatch(static_cast<contracts::Waveform>(k % 1000 % 3), -0.06 * m, 0.01 * m,
                                        0.01 * m, 0.001 * m, 0.01 * m, 20.0 + 19.98 * m, 0.1 + 0.0099 * m);
        return patch;
    };

    PatchState state(patchFor(0));
    std::atomic<bool> writing { true };

    std::thread writer([&] {
        for (int k = 1; k <= 200000; ++k)
            state.publish(patchFor(k));

        writing = false;
    });

    int successes = 0;
    int mixtures = 0;

    for (int attempt = 0; attempt < 2000000 || writing; ++attempt)
    {
        contracts::InstrumentPatch read;

        if (! state.tryRead(read))
            continue;

        ++successes;
        const int m = static_cast<int>(std::lround(read.attackSeconds / 0.01));
        const auto expected = patchFor(m);

        if (! (read == expected))
            ++mixtures;

        if (! writing && attempt >= 2000000)
            break;
    }

    writer.join();
    CHECK(successes > 0);
    CHECK(mixtures == 0);
}
