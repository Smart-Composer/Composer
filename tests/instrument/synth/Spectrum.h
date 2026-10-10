#pragma once

#include <vector>

namespace composer::instrument::synth::testing
{

/** Magnitudes of bins 0..N/2 of a real signal whose length is a power of two. */
std::vector<double> magnitudeSpectrum(const std::vector<double>& signal);

/** Applies a four-term Blackman-Harris window in place. */
void applyBlackmanHarris(std::vector<double>& signal);

} // namespace composer::instrument::synth::testing
