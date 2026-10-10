#pragma once

#include <composer_v1/contracts/InstrumentPatch.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace composer_v1::instrument::synth
{

/** Band-limited single-cycle tables for every waveform, built once per process.

    Saw and square each have one table per harmonic limit, a third of an octave apart; a note
    plays the richest table whose highest harmonic stays below the Nyquist frequency. Every table
    starts at a zero crossing.
*/
class WavetableBank
{
public:
    static constexpr int tableSize = 4096;
    static constexpr int phaseBits = 20; // 32-bit phase: 12 index bits, 20 fraction bits

    /** Highest harmonic of each saw and square table, round(2^(k/3)) without repeats. */
    static constexpr std::array<int, 28> harmonicLimits {
        1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 20, 25, 32, 40,
        51, 64, 81, 102, 128, 161, 203, 256, 323, 406, 512, 645, 813, 1024
    };

    static const WavetableBank& instance();

    /** The table index for a fundamental at a sample rate, or -1 when the fundamental is at or
        above the Nyquist frequency. */
    static int selectTable(double fundamentalHz, double sampleRate) noexcept;

    /** The table for a waveform and table index. Valid samples run from [-1] to [tableSize + 1]
        so that four-point interpolation never wraps. Sine has a single table. */
    const float* table(contracts::Waveform waveform, int tableIndex) const noexcept;

private:
    WavetableBank();

    static constexpr std::size_t stride = static_cast<std::size_t>(tableSize) + 3;

    std::vector<float> storage;
};

/** Four-point Catmull-Rom interpolation at a 32-bit phase. */
inline double readTable(const float* table, std::uint32_t phase) noexcept
{
    const auto index = static_cast<int>(phase >> WavetableBank::phaseBits);
    const double fraction = static_cast<double>(phase & ((1u << WavetableBank::phaseBits) - 1u))
                          * (1.0 / static_cast<double>(1u << WavetableBank::phaseBits));

    const double y0 = table[index - 1];
    const double y1 = table[index];
    const double y2 = table[index + 1];
    const double y3 = table[index + 2];

    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);

    return ((c3 * fraction + c2) * fraction + c1) * fraction + y1;
}

} // namespace composer_v1::instrument::synth
