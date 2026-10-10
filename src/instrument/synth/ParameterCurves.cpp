#include <composer/instrument/synth/ParameterCurves.h>

#include <algorithm>
#include <cmath>

namespace composer::instrument::synth
{

double denormalise(std::size_t index, float normalised) noexcept
{
    const auto& descriptor = contracts::parameterDescriptors[index];

    if (! (normalised > 0.0f))
        return descriptor.minimum;

    if (normalised >= 1.0f)
        return descriptor.maximum;

    const double x = normalised;
    const double span = descriptor.maximum - descriptor.minimum;
    double value = descriptor.minimum;

    switch (parameterCurves[index].curve)
    {
        case Curve::choice:
            value = descriptor.minimum + std::round(x * span);
            break;
        case Curve::linear:
            value = descriptor.minimum + span * x;
            break;
        case Curve::cubic:
            value = descriptor.minimum + span * x * x * x;
            break;
        case Curve::logarithmic:
            value = descriptor.minimum * std::exp(x * std::log(descriptor.maximum / descriptor.minimum));
            break;
    }

    return std::clamp(value, descriptor.minimum, descriptor.maximum);
}

float normalise(std::size_t index, double value) noexcept
{
    const auto& descriptor = contracts::parameterDescriptors[index];

    if (! (value > descriptor.minimum))
        return 0.0f;

    if (value >= descriptor.maximum)
        return 1.0f;

    const double span = descriptor.maximum - descriptor.minimum;
    double normalised = 0.0;

    switch (parameterCurves[index].curve)
    {
        case Curve::choice:
            normalised = (std::round(value) - descriptor.minimum) / span;
            break;
        case Curve::linear:
            normalised = (value - descriptor.minimum) / span;
            break;
        case Curve::cubic:
            normalised = std::cbrt((value - descriptor.minimum) / span);
            break;
        case Curve::logarithmic:
            normalised = std::log(value / descriptor.minimum)
                       / std::log(descriptor.maximum / descriptor.minimum);
            break;
    }

    const auto rounded = static_cast<float>(std::clamp(normalised, 0.0, 1.0));
    return rounded == 0.0f ? 0.0f : rounded;
}

double fieldValue(const contracts::InstrumentPatch& patch, std::size_t index) noexcept
{
    const auto& descriptor = contracts::parameterDescriptors[index];

    if (descriptor.member == nullptr)
        return static_cast<double>(patch.waveform);

    return patch.*descriptor.member;
}

void setFieldValue(contracts::InstrumentPatch& patch, std::size_t index, double value) noexcept
{
    const auto& descriptor = contracts::parameterDescriptors[index];

    if (descriptor.member == nullptr)
    {
        const auto choice = std::clamp(std::lround(value), 0L,
                                       static_cast<long>(contracts::waveformDescriptors.size()) - 1L);
        patch.waveform = contracts::waveformDescriptors[static_cast<std::size_t>(choice)].value;
        return;
    }

    patch.*descriptor.member = value;
}

} // namespace composer::instrument::synth
