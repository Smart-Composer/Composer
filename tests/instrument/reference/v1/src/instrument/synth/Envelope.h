#pragma once

#include <composer_v1/instrument/synth/Synth.h>

#include <cmath>

namespace composer_v1::instrument::synth
{

/** Per-sample rates for the attack-decay-sustain-release envelope at one sample rate.

    Attack is a linear rise. Decay and release are exponential and aim slightly past their
    targets, so they arrive in a finite, predictable time: the decay or release time is the time
    a full 1-to-0 fall takes. A stage shorter than one sample, including a zero time, is
    immediate.
*/
struct EnvelopeRates
{
    static constexpr double overshoot = 1.0e-4;

    bool immediateAttack = true;
    bool immediateDecay = true;
    bool immediateRelease = true;
    double attackIncrement = 1.0;
    double decayCoefficient = 0.0;
    double releaseCoefficient = 0.0;

    void set(double attackSeconds, double decaySeconds, double releaseSeconds, double sampleRate) noexcept
    {
        const double logRatio = std::log((1.0 + overshoot) / overshoot);

        const double attackSamples = attackSeconds * sampleRate;
        immediateAttack = ! (attackSamples >= 1.0);
        attackIncrement = immediateAttack ? 1.0 : 1.0 / attackSamples;

        const double decaySamples = decaySeconds * sampleRate;
        immediateDecay = ! (decaySamples >= 1.0);
        decayCoefficient = immediateDecay ? 0.0 : std::exp(-logRatio / decaySamples);

        const double releaseSamples = releaseSeconds * sampleRate;
        immediateRelease = ! (releaseSamples >= 1.0);
        releaseCoefficient = immediateRelease ? 0.0 : std::exp(-logRatio / releaseSamples);
    }
};

/** One voice's envelope position. */
struct EnvelopeState
{
    VoiceStage stage = VoiceStage::idle;
    double level = 0.0;

    /** Advances one sample at the given sustain level and returns the new level. */
    double tick(const EnvelopeRates& rates, double sustainLevel) noexcept
    {
        switch (stage)
        {
            case VoiceStage::idle:
                return 0.0;

            case VoiceStage::attack:
                if (rates.immediateAttack)
                {
                    level = 1.0;
                }
                else
                {
                    level += rates.attackIncrement;

                    if (level < 1.0)
                        return level;

                    level = 1.0;
                }

                stage = VoiceStage::decay;

                if (! rates.immediateDecay)
                    return level;

                level = sustainLevel;
                stage = VoiceStage::sustain;
                return level;

            case VoiceStage::decay:
                if (rates.immediateDecay)
                {
                    level = sustainLevel;
                    stage = VoiceStage::sustain;
                    return level;
                }

                {
                    const double aim = sustainLevel - EnvelopeRates::overshoot;
                    level = aim + (level - aim) * rates.decayCoefficient;
                }

                if (level <= sustainLevel)
                {
                    level = sustainLevel;
                    stage = VoiceStage::sustain;
                }

                return level;

            case VoiceStage::sustain:
                level = sustainLevel;
                return level;

            case VoiceStage::release:
                if (rates.immediateRelease)
                {
                    level = 0.0;
                    stage = VoiceStage::idle;
                    return level;
                }

                level = -EnvelopeRates::overshoot + (level + EnvelopeRates::overshoot) * rates.releaseCoefficient;

                if (level <= 0.0)
                {
                    level = 0.0;
                    stage = VoiceStage::idle;
                }

                return level;

            case VoiceStage::fading:
                return level;
        }

        return 0.0;
    }
};

} // namespace composer_v1::instrument::synth
