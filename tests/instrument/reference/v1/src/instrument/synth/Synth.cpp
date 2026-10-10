#include <composer_v1/instrument/synth/Synth.h>

#include "Envelope.h"
#include "LinearSmoother.h"
#include "StateVariableFilter.h"
#include "Wavetables.h"

#include <algorithm>
#include <cmath>

namespace composer_v1::instrument::synth
{
namespace
{

double decibelsToGain(double decibels) noexcept
{
    return std::exp(decibels * (std::log(10.0) / 20.0));
}

struct Voice
{
    EnvelopeState envelope;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    bool sounded = false;
    int fadeRemaining = 0;
    std::uint64_t startOrder = 0;
    contracts::Waveform waveform = contracts::Waveform::sine;
    const float* table = nullptr;
    std::uint32_t phase = 0;
    std::uint32_t increment = 0;
    double velocityGain = 0.0;
    double velocityTarget = 0.0;
    double velocityStep = 0.0;
    int velocityRemaining = 0;
    SvfState filter;

    bool isIdle() const noexcept
    {
        return envelope.stage == VoiceStage::idle;
    }

    bool isFading() const noexcept
    {
        return envelope.stage == VoiceStage::fading;
    }

    /** Held or releasing: a note that still belongs to its key. */
    bool isSounding() const noexcept
    {
        return ! isIdle() && ! isFading();
    }

    bool isHeld() const noexcept
    {
        return isSounding() && envelope.stage != VoiceStage::release;
    }

    double nextVelocityGain() noexcept
    {
        if (velocityRemaining > 0)
        {
            --velocityRemaining;
            velocityGain = velocityRemaining == 0 ? velocityTarget : velocityGain + velocityStep;
        }

        return velocityGain;
    }

    double nextOscillator() noexcept
    {
        const double sample = table != nullptr ? readTable(table, phase) : 0.0;
        phase += increment;
        return sample;
    }
};

} // namespace

struct Synth::State
{
    const WavetableBank& bank = WavetableBank::instance();

    bool prepared = false;
    double sampleRate = 0.0;
    int fadeLength = 1;
    double maximumCutoff = 0.0;

    std::array<std::uint32_t, 128> increments {};
    std::array<int, 128> tableIndices {};

    std::array<Voice, voiceSlots> voices {};
    std::uint64_t nextStartOrder = 0;

    contracts::Waveform waveform = contracts::Waveform::sine;
    double attackSeconds = -1.0;
    double decaySeconds = -1.0;
    double releaseSeconds = -1.0;
    EnvelopeRates rates;

    LinearSmoother gainDb;
    LinearSmoother sustain;
    LinearSmoother logCutoff;
    LinearSmoother logQ;
    double linearGain = 1.0;
    SvfCoefficients filter;

    double effectiveCutoff() const noexcept
    {
        return std::min(std::exp2(logCutoff.value()), maximumCutoff);
    }

    void updateFilter() noexcept
    {
        filter = SvfCoefficients::lowPass(effectiveCutoff(), std::exp(logQ.value()), sampleRate);
    }

    void snapSmoothers() noexcept
    {
        gainDb.snap();
        sustain.snap();
        logCutoff.snap();
        logQ.snap();
        linearGain = decibelsToGain(gainDb.value());

        if (prepared)
            updateFilter();
    }

    bool anySmootherRamping() const noexcept
    {
        return gainDb.isRamping() || sustain.isRamping() || logCutoff.isRamping() || logQ.isRamping();
    }

    bool anyVoiceBusy() const noexcept
    {
        for (const auto& voice : voices)
            if (! voice.isIdle())
                return true;

        return false;
    }

    int soundingCount() const noexcept
    {
        int count = 0;

        for (const auto& voice : voices)
            count += voice.isSounding() ? 1 : 0;

        return count;
    }

    Voice* freeVoice() noexcept
    {
        for (auto& voice : voices)
            if (voice.isIdle())
                return &voice;

        return nullptr;
    }

    Voice* soundingVoice(int channel, int note) noexcept
    {
        for (auto& voice : voices)
            if (voice.isSounding() && voice.channel == channel && voice.note == note)
                return &voice;

        return nullptr;
    }

    /** The quietest releasing note, or else the oldest held one. */
    Voice& stealVictim() noexcept
    {
        Voice* victim = nullptr;

        for (auto& voice : voices)
        {
            if (! voice.isSounding() || voice.envelope.stage != VoiceStage::release)
                continue;

            if (victim == nullptr)
            {
                victim = &voice;
                continue;
            }

            const double loudness = voice.envelope.level * voice.velocityGain;
            const double victimLoudness = victim->envelope.level * victim->velocityGain;

            if (loudness < victimLoudness || (loudness == victimLoudness && voice.startOrder < victim->startOrder))
                victim = &voice;
        }

        if (victim != nullptr)
            return *victim;

        for (auto& voice : voices)
            if (voice.isSounding() && (victim == nullptr || voice.startOrder < victim->startOrder))
                victim = &voice;

        return *victim;
    }

    void clearVoice(Voice& voice) noexcept
    {
        voice = Voice {};
    }

    /** Fades a note out from where it is. A note that has not yet produced a sample is simply
        dropped: there is nothing to fade. */
    void beginFade(Voice& voice) noexcept
    {
        if (! voice.sounded)
        {
            clearVoice(voice);
            return;
        }

        voice.envelope.stage = VoiceStage::fading;
        voice.fadeRemaining = fadeLength;
    }

    /** Starts a note in place of a sounding voice, which fades out. When the old voice needs a
        fade and no voice is free, the note is ignored and nothing changes. */
    void replaceVoice(Voice& old, int channel, int note, double velocityGain) noexcept
    {
        if (! old.sounded)
        {
            startVoice(old, channel, note, velocityGain);
            return;
        }

        Voice* free = freeVoice();

        if (free == nullptr)
            return;

        beginFade(old);
        startVoice(*free, channel, note, velocityGain);
    }

    void startVoice(Voice& voice, int channel, int note, double velocityGain) noexcept
    {
        clearVoice(voice);
        voice.envelope.stage = VoiceStage::attack;
        voice.channel = static_cast<std::uint8_t>(channel);
        voice.note = static_cast<std::uint8_t>(note);
        voice.startOrder = nextStartOrder++;
        voice.waveform = waveform;
        voice.velocityGain = velocityGain;
        voice.velocityTarget = velocityGain;

        const int tableIndex = tableIndices[static_cast<std::size_t>(note)];
        voice.table = tableIndex < 0 ? nullptr : bank.table(waveform, tableIndex);
        voice.increment = voice.table != nullptr ? increments[static_cast<std::size_t>(note)] : 0u;

        // Every waveform starts at a zero of its table, so a note's first sample is exactly 0.
        voice.phase = waveform == contracts::Waveform::saw ? 0x80000000u : 0u;
    }

    double renderVoice(Voice& voice, double sustainLevel) noexcept
    {
        const double oscillator = voice.nextOscillator();
        const double filtered = voice.filter.processLowPass(oscillator, filter);
        const double envelope = voice.envelope.tick(rates, sustainLevel);
        const double velocity = voice.nextVelocityGain();
        const double sample = filtered * envelope * velocity;
        voice.sounded = true;

        if (voice.isIdle())
            clearVoice(voice);

        return sample;
    }

    /** A fading note keeps its level and velocity and ramps linearly to exactly zero. */
    double renderFade(Voice& voice) noexcept
    {
        const double oscillator = voice.nextOscillator();
        const double filtered = voice.filter.processLowPass(oscillator, filter);

        --voice.fadeRemaining;
        const double fade = static_cast<double>(voice.fadeRemaining) / static_cast<double>(fadeLength);
        const double sample = filtered * voice.envelope.level * voice.velocityGain * fade;

        if (voice.fadeRemaining == 0)
            clearVoice(voice);

        return sample;
    }
};

Synth::Synth()
    : state(std::make_unique<State>())
{
}

Synth::~Synth() = default;

void Synth::prepare(double sampleRate, const contracts::InstrumentPatch& patch) noexcept
{
    auto& s = *state;

    if (! (std::isfinite(sampleRate) && sampleRate > 0.0))
    {
        s.prepared = false;
        return;
    }

    s.sampleRate = sampleRate;
    s.fadeLength = std::max(1, static_cast<int>(std::lround(declickSeconds * sampleRate)));
    s.maximumCutoff = maximumCutoffRatio * sampleRate;

    const int rampLength = std::max(1, static_cast<int>(std::lround(parameterRampSeconds * sampleRate)));
    s.gainDb.setLength(rampLength);
    s.sustain.setLength(rampLength);
    s.logCutoff.setLength(rampLength);
    s.logQ.setLength(rampLength);

    for (int note = 0; note < 128; ++note)
    {
        const double fundamental = 440.0 * std::exp2((note - 69) / 12.0);
        const int tableIndex = WavetableBank::selectTable(fundamental, sampleRate);
        s.tableIndices[static_cast<std::size_t>(note)] = tableIndex;
        s.increments[static_cast<std::size_t>(note)] =
            tableIndex < 0 ? 0u
                           : static_cast<std::uint32_t>(std::llround(fundamental / sampleRate * 4294967296.0));
    }

    s.prepared = true;
    s.attackSeconds = -1.0;
    stopAllVoicesNow();
    setTargets(patch);
    s.snapSmoothers();
}

bool Synth::isPrepared() const noexcept
{
    return state->prepared;
}

void Synth::setTargets(const contracts::InstrumentPatch& patch) noexcept
{
    auto& s = *state;

    s.gainDb.setTarget(patch.gainDb);
    s.sustain.setTarget(patch.sustainLevel);
    s.logCutoff.setTarget(std::log2(patch.cutoffHz));
    s.logQ.setTarget(std::log(patch.resonanceQ));
    s.waveform = patch.waveform;

    if (s.prepared
        && (patch.attackSeconds != s.attackSeconds || patch.decaySeconds != s.decaySeconds
            || patch.releaseSeconds != s.releaseSeconds))
    {
        s.attackSeconds = patch.attackSeconds;
        s.decaySeconds = patch.decaySeconds;
        s.releaseSeconds = patch.releaseSeconds;
        s.rates.set(s.attackSeconds, s.decaySeconds, s.releaseSeconds, s.sampleRate);
    }

    if (! s.anyVoiceBusy())
        s.snapSmoothers();
}

void Synth::handleMidi(const std::uint8_t* data, int numBytes) noexcept
{
    if (data == nullptr || numBytes < 3)
        return;

    const int status = data[0];

    if (status < 0x80 || status >= 0xF0)
        return;

    const int channel = status & 0x0F;
    const int first = data[1] & 0x7F;
    const int second = data[2] & 0x7F;

    switch (status & 0xF0)
    {
        case 0x90:
            if (second > 0)
                noteOn(channel, first, second);
            else
                noteOff(channel, first);
            break;
        case 0x80:
            noteOff(channel, first);
            break;
        case 0xB0:
            if (first == 120)
                allSoundOff(channel);
            else if (first == 123)
                allNotesOff(channel);
            break;
        default:
            break;
    }
}

void Synth::noteOn(int channel, int note, int velocity) noexcept
{
    auto& s = *state;

    if (! s.prepared || channel < 0 || channel > 15 || note < 0 || note > 127 || velocity < 1)
        return;

    const double normalisedVelocity = static_cast<double>(std::min(velocity, 127)) / 127.0;
    const double velocityGain = normalisedVelocity * normalisedVelocity;

    if (Voice* same = s.soundingVoice(channel, note))
    {
        // An attack at least as long as the declick fade rises from the current level without a
        // step, so the voice can carry on.
        const bool attackDeclicks = ! s.rates.immediateAttack && s.rates.attackIncrement * s.fadeLength <= 1.0;

        if (same->waveform == s.waveform && attackDeclicks)
        {
            // Retrigger in place: attack from the current level, phase and filter continue. It
            // is now the newest note.
            same->envelope.stage = VoiceStage::attack;
            same->startOrder = s.nextStartOrder++;
            same->velocityTarget = velocityGain;
            same->velocityStep = (velocityGain - same->velocityGain) / static_cast<double>(s.fadeLength);
            same->velocityRemaining = s.fadeLength;
            return;
        }

        // The waveform changed after the note started, or the attack is too short to rise from
        // the current level without a click: the old voice fades and the note starts again.
        s.replaceVoice(*same, channel, note, velocityGain);
        return;
    }

    if (s.soundingCount() < polyphony)
    {
        if (Voice* free = s.freeVoice())
            s.startVoice(*free, channel, note, velocityGain);

        return;
    }

    s.replaceVoice(s.stealVictim(), channel, note, velocityGain);
}

void Synth::noteOff(int channel, int note) noexcept
{
    for (auto& voice : state->voices)
        if (voice.isHeld() && voice.channel == channel && voice.note == note)
            voice.envelope.stage = VoiceStage::release;
}

void Synth::allNotesOff(int channel) noexcept
{
    for (auto& voice : state->voices)
        if (voice.isHeld() && voice.channel == channel)
            voice.envelope.stage = VoiceStage::release;
}

void Synth::allSoundOff(int channel) noexcept
{
    for (auto& voice : state->voices)
        if (voice.isSounding() && voice.channel == channel)
            state->beginFade(voice);
}

void Synth::fadeOutAllVoices() noexcept
{
    for (auto& voice : state->voices)
        if (voice.isSounding())
            state->beginFade(voice);
}

void Synth::stopAllVoicesNow() noexcept
{
    auto& s = *state;

    for (auto& voice : s.voices)
        s.clearVoice(voice);

    s.nextStartOrder = 0;
    s.snapSmoothers();
}

void Synth::render(float* output, int numSamples) noexcept
{
    auto& s = *state;

    if (numSamples <= 0)
        return;

    if (! s.prepared)
    {
        if (output != nullptr)
            std::fill(output, output + numSamples, 0.0f);

        return;
    }

    for (int index = 0; index < numSamples; ++index)
    {
        if (s.anySmootherRamping())
        {
            // Nothing sounding means nothing can click: land every ramp at once, so the result
            // does not depend on where a render was split into blocks.
            if (! s.anyVoiceBusy())
            {
                s.snapSmoothers();
            }
            else
            {
                if (s.gainDb.isRamping())
                    s.linearGain = decibelsToGain(s.gainDb.next());

                s.sustain.next();

                if (s.logCutoff.isRamping() || s.logQ.isRamping())
                {
                    s.logCutoff.next();
                    s.logQ.next();
                    s.updateFilter();
                }
            }
        }

        const double sustainLevel = s.sustain.value();
        double sum = 0.0;

        for (auto& voice : s.voices)
        {
            if (voice.isFading())
                sum += s.renderFade(voice);
            else if (! voice.isIdle())
                sum += s.renderVoice(voice, sustainLevel);
        }

        double sample = sum * s.linearGain;

        if (std::abs(sample) < outputFlushThreshold)
            sample = 0.0;

        if (output != nullptr)
            output[index] = static_cast<float>(sample);
    }
}

int Synth::activeVoiceCount() const noexcept
{
    return state->soundingCount();
}

int Synth::fadingVoiceCount() const noexcept
{
    int count = 0;

    for (const auto& voice : state->voices)
        count += voice.isFading() ? 1 : 0;

    return count;
}

std::array<VoiceView, voiceSlots> Synth::voices() const noexcept
{
    std::array<VoiceView, voiceSlots> views {};

    for (std::size_t index = 0; index < views.size(); ++index)
    {
        const auto& voice = state->voices[index];
        views[index] = VoiceView { voice.envelope.stage, voice.channel, voice.note,
                                   voice.envelope.level, voice.startOrder, voice.waveform };
    }

    return views;
}

double Synth::effectiveCutoffHz() const noexcept
{
    return state->effectiveCutoff();
}

} // namespace composer_v1::instrument::synth
