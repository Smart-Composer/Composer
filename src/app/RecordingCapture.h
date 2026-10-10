#pragma once

#include "CaptureClock.h"

#include <composer/project/MidiCaptureBuffer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace composer::app
{
enum class CaptureInterruption : unsigned
{
    generationChanged = 1,
    invalidCorrection = 2,
    stalePublisher = 4,
    regressedSerial = 8,
    lateOwnerPoll = 16,
    ownerAbandoned = 32,
    neverAdmitted = 64,
    monitoringFailed = 128
};

struct PendingTake
{
    project::CapturedMidi captured;
    unsigned interruptionFaults = 0;
    // Timing projections preserve every accepted message's bytes and arrival
    // order. These counts are informational and do not imply lost events.
    std::uint64_t arrivalTimeAdjustments = 0;
    std::uint64_t preOriginEvents = 0;
    bool isComplete() const noexcept { return interruptionFaults == 0 && captured.isComplete(); }
};

// One immutable take, owned by one control thread. Only the MIDI callback and
// interrupt() may run concurrently with control operations. Prepare before
// attaching a callback. The publisher's actual signals must remain alive through
// sealAfterJoin(); after sealing, collection and destruction never read them.
// The owner polls often enough to prove publisher freshness, and invalidates its
// generation for device, transport, context, rate or origin changes.
class RecordingCapture final : public juce::MidiInputCallback
{
public:
    RecordingCapture(const CaptureClockSignals&, CaptureMapping, std::size_t capacity,
                     std::uint64_t nowMs, std::uint64_t freshnessLimitMs);
    ~RecordingCapture() override;
    RecordingCapture(const RecordingCapture&) = delete;
    RecordingCapture& operator=(const RecordingCapture&) = delete;
    RecordingCapture(RecordingCapture&&) = delete;
    RecordingCapture& operator=(RecordingCapture&&) = delete;

    bool open(std::uint64_t nowMs) noexcept;
    bool poll(std::uint64_t nowMs) noexcept;
    void close() noexcept;
    bool accepting() const noexcept { return accepting_.load(); }
    unsigned interruptionFaults() const noexcept { return faults_.load(); }
    void interrupt(CaptureInterruption) noexcept;

    // close() is an admission boundary, not a join. Already admitted callbacks
    // may finish afterward. Detach/join every producer before sealing, changing
    // the timestamp origin, or destroying this object. Sample nowMs after joining
    // and seal before intentionally invalidating/destroying the publisher.
    void sealAfterJoin(std::uint64_t nowMs, unsigned extraFaults = 0) noexcept;
    bool sealed() const noexcept { return sealed_; }

    // Requires sealing. Allocation failure retains all originals for retry.
    // Arrival order is preserved. A mapped time earlier than its predecessor is
    // raised to that predecessor's time; arrivalTimeAdjustments counts changes.
    // The returned reference is stable until this capture is destroyed.
    const PendingTake& collect();

    // No allocation, locks, waiting, I/O or live engine calls. Timestamp zero is
    // used literally; there is no now-clock fallback or latency compensation.
    // A valid stream sample before the take origin is retained at zero and
    // counted in preOriginEvents only if its channel message was accepted.
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) noexcept override;

private:
    bool clockMatches() noexcept;
    std::optional<double> admit() noexcept;
    double relativeSeconds(double rawTimestamp, double correction, bool& preOrigin) const noexcept;

    const CaptureClockSignals& clock_;
    const CaptureMapping mapping_;
    const std::uint64_t freshnessLimit_;
    const std::int64_t relativeOffset_;
    project::MidiCaptureBuffer buffer_;
    std::atomic<bool> accepting_{false};
    std::atomic<unsigned> faults_{0};
    std::atomic<std::uint64_t> preOriginEvents_{0};
    std::uint64_t lastSerial_, lastProgress_, lastPoll_;
    bool retired_ = false, opened_ = false, sealed_ = false;
    std::unique_ptr<PendingTake> pending_;
};

// Owns an already opened, STOPPED input with no existing callbacks. It never
// enumerates, opens or configures a device. The dedicated enabled virtual input
// must be a current DeviceManager member and receive no other physical source.
// Capture, its publisher and the engine outlive this guard. Construction, stop
// and destruction run on the message thread, never inside a MIDI callback.
class RecordingInput final : private juce::MidiInputCallback
{
public:
    using NowMilliseconds = std::uint64_t (*)() noexcept;
    static std::uint64_t currentMilliseconds() noexcept;

    RecordingInput(std::unique_ptr<juce::MidiInput> stoppedInput,
                   std::shared_ptr<tracktion::VirtualMidiInputDevice>, RecordingCapture&,
                   NowMilliseconds = &currentMilliseconds);
    ~RecordingInput() override;
    RecordingInput(const RecordingInput&) = delete;
    RecordingInput& operator=(const RecordingInput&) = delete;
    RecordingInput(RecordingInput&&) = delete;
    RecordingInput& operator=(RecordingInput&&) = delete;

    // Closes, joins, freezes final validity, then releases the input. If JUCE
    // unexpectedly throws before detach completes, ownership is retained for
    // retry. Destruction cannot continue past a failed join; it terminates rather
    // than destroy a callback whose registration may remain live.
    void stop();

private:
    void finish(unsigned extraFaults);
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) noexcept override;

    std::unique_ptr<juce::MidiInput> input_;
    const std::shared_ptr<tracktion::VirtualMidiInputDevice> destination_;
    RecordingCapture& capture_;
    const NowMilliseconds now_;
    bool finished_ = false;
};
}
