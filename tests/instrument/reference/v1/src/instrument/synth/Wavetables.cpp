#include "Wavetables.h"

#include <cmath>
#include <numbers>

namespace composer_v1::instrument::synth
{
namespace
{

constexpr int sawTables = static_cast<int>(WavetableBank::harmonicLimits.size());

// Table layout in storage: sine, then the saw tables, then the square tables.
std::size_t tableOffset(contracts::Waveform waveform, int tableIndex) noexcept
{
    switch (waveform)
    {
        case contracts::Waveform::sine:
            return 0;
        case contracts::Waveform::saw:
            return static_cast<std::size_t>(1 + tableIndex);
        case contracts::Waveform::square:
            return static_cast<std::size_t>(1 + sawTables + tableIndex);
    }

    return 0;
}

/** sin(2 pi k / N) for k in [0, N), exact at the quarter points so that every table built from
    it has exact zeros at phases 0 and 1/2. */
std::vector<double> sineCycle()
{
    constexpr int size = WavetableBank::tableSize;
    constexpr int quarter = size / 4;
    std::vector<double> cycle(static_cast<std::size_t>(size));

    for (int k = 0; k <= quarter; ++k)
    {
        const double value = k == quarter ? 1.0
                                          : std::sin(2.0 * std::numbers::pi * k / size);
        cycle[static_cast<std::size_t>(k)] = value;
        cycle[static_cast<std::size_t>(2 * quarter - k)] = value;
        cycle[static_cast<std::size_t>((2 * quarter + k) % size)] = -value;
        cycle[static_cast<std::size_t>((size - k) % size)] = -value;
    }

    cycle[0] = 0.0;
    cycle[static_cast<std::size_t>(2 * quarter)] = 0.0;
    return cycle;
}

void storeTable(std::vector<float>& storage, std::size_t offset, const std::vector<double>& cycle)
{
    constexpr int size = WavetableBank::tableSize;
    float* destination = storage.data() + offset * (static_cast<std::size_t>(size) + 3);

    destination[0] = static_cast<float>(cycle[size - 1]);
    for (int k = 0; k < size; ++k)
        destination[1 + k] = static_cast<float>(cycle[static_cast<std::size_t>(k)]);
    destination[size + 1] = static_cast<float>(cycle[0]);
    destination[size + 2] = static_cast<float>(cycle[1]);
}

} // namespace

const WavetableBank& WavetableBank::instance()
{
    static const WavetableBank bank;
    return bank;
}

WavetableBank::WavetableBank()
    : storage(stride * static_cast<std::size_t>(1 + 2 * sawTables))
{
    constexpr int size = tableSize;
    const auto sine = sineCycle();
    storeTable(storage, tableOffset(contracts::Waveform::sine, 0), sine);

    // Saw: -(2/pi) sum sin(2 pi n phi) / n, rising through zero at phi = 1/2.
    // Square: (4/pi) sum over odd n of sin(2 pi n phi) / n, zero at phi = 0.
    std::vector<double> saw(static_cast<std::size_t>(size), 0.0);
    std::vector<double> square(static_cast<std::size_t>(size), 0.0);
    int harmonic = 0;

    for (int tableIndex = 0; tableIndex < sawTables; ++tableIndex)
    {
        for (++harmonic; harmonic <= harmonicLimits[static_cast<std::size_t>(tableIndex)]; ++harmonic)
        {
            const double sawWeight = -2.0 / (std::numbers::pi * harmonic);
            const double squareWeight = (harmonic % 2 == 1) ? 4.0 / (std::numbers::pi * harmonic) : 0.0;

            for (int k = 0; k < size; ++k)
            {
                const double value = sine[static_cast<std::size_t>((static_cast<long long>(harmonic) * k) % size)];
                saw[static_cast<std::size_t>(k)] += sawWeight * value;
                square[static_cast<std::size_t>(k)] += squareWeight * value;
            }
        }

        --harmonic;
        storeTable(storage, tableOffset(contracts::Waveform::saw, tableIndex), saw);
        storeTable(storage, tableOffset(contracts::Waveform::square, tableIndex), square);
    }
}

int WavetableBank::selectTable(double fundamentalHz, double sampleRate) noexcept
{
    const double nyquist = 0.5 * sampleRate;

    if (! (fundamentalHz > 0.0) || fundamentalHz >= nyquist)
        return -1;

    // The highest harmonic strictly below the Nyquist frequency.
    auto highest = static_cast<long long>(std::ceil(nyquist / fundamentalHz)) - 1;
    while (highest > 0 && static_cast<double>(highest) * fundamentalHz >= nyquist)
        --highest;
    while (static_cast<double>(highest + 1) * fundamentalHz < nyquist)
        ++highest;

    int selected = 0;
    for (int index = 0; index < sawTables; ++index)
        if (harmonicLimits[static_cast<std::size_t>(index)] <= highest)
            selected = index;

    return selected;
}

const float* WavetableBank::table(contracts::Waveform waveform, int tableIndex) const noexcept
{
    const auto index = waveform == contracts::Waveform::sine ? 0 : tableIndex;
    return storage.data() + tableOffset(waveform, index) * stride + 1;
}

} // namespace composer_v1::instrument::synth
