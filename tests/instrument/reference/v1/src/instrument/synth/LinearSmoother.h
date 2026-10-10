#pragma once

namespace composer_v1::instrument::synth
{

/** A linear ramp to a target over a fixed number of samples that lands exactly on the target.

    Unlike an approximately-equal shortcut, any changed target restarts the ramp from the current
    value, so a change of a single unit in the last place is still applied.
*/
class LinearSmoother
{
public:
    void setLength(int samples) noexcept
    {
        length = samples < 1 ? 1 : samples;
    }

    void setTarget(double newTarget) noexcept
    {
        if (newTarget == target)
            return;

        target = newTarget;

        if (length <= 1)
        {
            snap();
            return;
        }

        step = (target - current) / static_cast<double>(length);
        remaining = length;
    }

    void snap() noexcept
    {
        current = target;
        remaining = 0;
    }

    bool isRamping() const noexcept
    {
        return remaining > 0;
    }

    double value() const noexcept
    {
        return current;
    }

    double targetValue() const noexcept
    {
        return target;
    }

    /** Advances one sample and returns the new value. */
    double next() noexcept
    {
        if (remaining > 0)
        {
            --remaining;
            current = remaining == 0 ? target : current + step;
        }

        return current;
    }

private:
    double current = 0.0;
    double target = 0.0;
    double step = 0.0;
    int remaining = 0;
    int length = 1;
};

} // namespace composer_v1::instrument::synth
