#pragma once

#include <composer/contracts/InstrumentPatch.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace composer::instrument::synth
{

inline constexpr std::size_t parameterCount = contracts::parameterDescriptors.size();

/** How a host's normalised 0..1 value maps onto a parameter's physical range. */
enum class Curve
{
    choice,
    linear,
    cubic,
    logarithmic
};

struct CurveSpec
{
    std::string_view id;
    Curve curve;
};

/** The normalisation curve of every patch parameter, in descriptor order.

    Hosts store automation as normalised values, so these shapes are persistent: changing one
    remaps every saved automation lane and session.
*/
inline constexpr std::array<CurveSpec, parameterCount> parameterCurves {{
    { "waveform", Curve::choice },
    { "gain_db", Curve::linear },
    { "attack_seconds", Curve::cubic },
    { "decay_seconds", Curve::cubic },
    { "sustain_level", Curve::linear },
    { "release_seconds", Curve::cubic },
    { "cutoff_hz", Curve::logarithmic },
    { "resonance_q", Curve::logarithmic },
}};

constexpr bool curvesMatchDescriptors()
{
    for (std::size_t index = 0; index < parameterCount; ++index)
    {
        const auto& descriptor = contracts::parameterDescriptors[index];
        const auto& spec = parameterCurves[index];

        if (spec.id != descriptor.id)
            return false;

        if ((spec.curve == Curve::choice) != (descriptor.kind == contracts::ParameterKind::choice))
            return false;

        if (spec.curve == Curve::logarithmic && ! (descriptor.minimum > 0.0))
            return false;
    }

    return true;
}

static_assert(curvesMatchDescriptors(),
              "Every patch parameter needs a normalisation curve, in descriptor order");

/** The physical value of a normalised host value. Out-of-range input clamps to the bounds, and
    the result always passes contracts::validatePatch. */
double denormalise(std::size_t index, float normalised) noexcept;

/** The normalised host value of a physical value, rounded to float once. */
float normalise(std::size_t index, double value) noexcept;

/** A patch field as a double, in descriptor order; the waveform reads as its choice index. */
double fieldValue(const contracts::InstrumentPatch& patch, std::size_t index) noexcept;

/** Writes a patch field from a double, in descriptor order; the waveform takes a choice index. */
void setFieldValue(contracts::InstrumentPatch& patch, std::size_t index, double value) noexcept;

} // namespace composer::instrument::synth
