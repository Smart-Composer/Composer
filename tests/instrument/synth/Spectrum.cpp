#include "Spectrum.h"

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <utility>

namespace composer::instrument::synth::testing
{

std::vector<double> magnitudeSpectrum(const std::vector<double>& signal)
{
    const std::size_t size = signal.size();
    std::vector<std::complex<double>> data(signal.begin(), signal.end());

    for (std::size_t i = 1, j = 0; i < size; ++i)
    {
        std::size_t bit = size >> 1;

        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;

        j ^= bit;

        if (i < j)
            std::swap(data[i], data[j]);
    }

    for (std::size_t length = 2; length <= size; length <<= 1)
    {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));

        for (std::size_t start = 0; start < size; start += length)
        {
            std::complex<double> twiddle(1.0, 0.0);

            for (std::size_t k = 0; k < length / 2; ++k)
            {
                const auto even = data[start + k];
                const auto odd = data[start + k + length / 2] * twiddle;
                data[start + k] = even + odd;
                data[start + k + length / 2] = even - odd;
                twiddle *= step;
            }
        }
    }

    std::vector<double> magnitudes(size / 2 + 1);

    for (std::size_t bin = 0; bin < magnitudes.size(); ++bin)
        magnitudes[bin] = std::abs(data[bin]);

    return magnitudes;
}

void applyBlackmanHarris(std::vector<double>& signal)
{
    const double n = static_cast<double>(signal.size());

    for (std::size_t index = 0; index < signal.size(); ++index)
    {
        const double x = 2.0 * std::numbers::pi * static_cast<double>(index) / n;
        signal[index] *= 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) - 0.01168 * std::cos(3.0 * x);
    }
}

} // namespace composer::instrument::synth::testing
