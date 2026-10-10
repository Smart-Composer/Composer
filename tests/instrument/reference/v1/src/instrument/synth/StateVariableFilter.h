#pragma once

#include <cmath>
#include <numbers>

namespace composer_v1::instrument::synth
{

/** Coefficients of a two-pole topology-preserving state-variable low-pass filter.

    The cutoff is pre-warped, so the response at the cutoff is exact at any sample rate. The DC
    gain is 1 and the gain at the cutoff equals Q.
*/
struct SvfCoefficients
{
    double g = 0.0;
    double gk = 0.0;
    double h = 1.0;

    static SvfCoefficients lowPass(double cutoffHz, double q, double sampleRate) noexcept
    {
        SvfCoefficients coefficients;
        const double k = 1.0 / q;
        coefficients.g = std::tan(std::numbers::pi * cutoffHz / sampleRate);
        coefficients.gk = coefficients.g + k;
        coefficients.h = 1.0 / (1.0 + coefficients.g * coefficients.gk);
        return coefficients;
    }
};

/** One filter's two integrator states. */
struct SvfState
{
    double s1 = 0.0;
    double s2 = 0.0;

    double processLowPass(double input, const SvfCoefficients& c) noexcept
    {
        const double highPass = c.h * (input - c.gk * s1 - s2);
        const double bandPass = c.g * highPass + s1;
        s1 = c.g * highPass + bandPass;
        const double lowPass = c.g * bandPass + s2;
        s2 = c.g * bandPass + lowPass;
        return lowPass;
    }

    void reset() noexcept
    {
        s1 = 0.0;
        s2 = 0.0;
    }
};

} // namespace composer_v1::instrument::synth
