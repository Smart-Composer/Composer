#pragma once

#include <composer_v1/contracts/InstrumentPatch.h>

#include <array>
#include <cstdint>
#include <memory>

namespace composer_v1::instrument::synth
{

/** Notes that sound at once. */
inline constexpr int polyphony = 16;

/** Voices in all: the sounding notes plus room for notes fading out. */
inline constexpr int voiceSlots = 2 * polyphony;

inline constexpr double parameterRampSeconds = 0.020;
inline constexpr double declickSeconds = 0.003;
inline constexpr double maximumCutoffRatio = 0.49;
inline constexpr double outputFlushThreshold = 1.0e-30;
inline constexpr double tailSeconds = 10.0 + declickSeconds;

enum class VoiceStage : std::uint8_t
{
    idle,
    attack,
    decay,
    sustain,
    release,
    fading
};

/** A read-only view of one voice, for inspection and tests. */
struct VoiceView
{
    VoiceStage stage = VoiceStage::idle;
    int channel = 0;
    int note = 0;
    double level = 0.0;
    std::uint64_t startOrder = 0;
    contracts::Waveform waveform = contracts::Waveform::sine;
};

/** The shared subtractive synthesiser: band-limited oscillator, two-pole low-pass filter,
    envelope and gain, sixteen notes at once.

    A note that is taken for a new one, stopped, or restarted (with a new waveform, or an attack
    too short to rise from its current level) fades out over 3 ms in place, so nothing that has
    sounded stops abruptly. A note that has not yet produced a sample needs no fade. A note-on
    that finds none of the 32 voices free is ignored, leaving every sounding note as it was.

    Every method after construction is real-time safe: no allocation, no locks, no exceptions.
    With a fixed patch, output depends only on the sample rate and the timing of events in
    samples, never on how a render is split into blocks.
*/
class Synth
{
public:
    Synth();
    ~Synth();

    Synth(const Synth&) = delete;
    Synth& operator=(const Synth&) = delete;

    /** Sets the sample rate and patch, and silences every voice. A rate that is not finite and
        positive leaves the synthesiser unprepared, rendering silence. */
    void prepare(double sampleRate, const contracts::InstrumentPatch& patch) noexcept;
    bool isPrepared() const noexcept;

    /** Moves towards a new patch. Gain, sustain, cutoff and resonance ramp; envelope times
        apply at once; the waveform applies to notes started afterwards. */
    void setTargets(const contracts::InstrumentPatch& patch) noexcept;

    /** Applies one raw MIDI message: note on and off, all-notes-off (CC 123) and all-sound-off
        (CC 120), each for its own channel. Everything else is ignored. */
    void handleMidi(const std::uint8_t* data, int numBytes) noexcept;

    /** channel is the MIDI channel index 0-15; velocity 1-127. */
    void noteOn(int channel, int note, int velocity) noexcept;
    void noteOff(int channel, int note) noexcept;

    /** Releases every held note on a channel, as its note-offs would. */
    void allNotesOff(int channel) noexcept;

    /** Fades out every note on a channel over 3 ms, with no release. */
    void allSoundOff(int channel) noexcept;

    /** Fades out every note on every channel over 3 ms, with no release. */
    void fadeOutAllVoices() noexcept;

    /** Silences every voice at once and resets all signal state, for a break in the audio. */
    void stopAllVoicesNow() noexcept;

    /** Renders and overwrites numSamples samples. A null output still advances the state. */
    void render(float* output, int numSamples) noexcept;

    /** Notes sounding, held or releasing, not counting those fading out. */
    int activeVoiceCount() const noexcept;
    int fadingVoiceCount() const noexcept;
    std::array<VoiceView, voiceSlots> voices() const noexcept;
    double effectiveCutoffHz() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state;
};

} // namespace composer_v1::instrument::synth
