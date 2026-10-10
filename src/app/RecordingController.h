#pragma once

#include "ApplicationProject.h"
#include "RecordingCapture.h"
#include "RecordingDuration.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>

namespace composer::app
{
enum class RecordingState { idle, preparing, recording, stopping, pendingCompletion };
enum class RecordingFailure
{
    none, busy, invalidInput, unavailablePublisher, preparationTimedOut,
    contextChanged, captureInterrupted, unavailableEndpoint, invalidDuration,
    projectRejected, exception
};

struct RecordingStatus
{
    RecordingState state = RecordingState::idle;
    RecordingFailure failure = RecordingFailure::none;
    std::optional<CompletionError> durationError;
    std::optional<contracts::EditOutcome> completed;
    std::uint64_t arrivalTimeAdjustments = 0, preOriginEvents = 0;
};

// Message-thread owner for one recording attempt. ApplicationProject and its
// engine outlive this controller. The controller exclusively owns its transport,
// graph changes and the global output processor slot while busy. External device,
// rate, seek or context changes must first stop this attempt; polling also fails
// closed on observed drift. No method runs on an audio or MIDI callback thread.
class RecordingController final
{
public:
    RecordingController(ApplicationProject&, std::size_t capacity,
                        std::uint64_t freshnessLimitMs = 1000,
                        std::uint64_t preparationTimeoutMs = 5000,
                        RecordingInput::NowMilliseconds = &RecordingInput::currentMilliseconds);
    // Resolve or explicitly discard a retained take before destruction. Fallback
    // shutdown joins callbacks and marks abandonment; it never commits a take or
    // cancels an admitted ticket. Originals cannot outlive this object.
    ~RecordingController();
    RecordingController(const RecordingController&) = delete;
    RecordingController& operator=(const RecordingController&) = delete;

    // Takes an already opened STOPPED input with no callbacks and a current,
    // enabled dedicated virtual input with monitoring on, useAllInputs=false
    // and no sources.
    // Selection, physical opening and settings changes belong to the caller.
    // Success starts asynchronous preparation; poll until recording or failure.
    bool start(std::unique_ptr<juce::MidiInput>, std::shared_ptr<tracktion::VirtualMidiInputDevice>);
    bool poll();
    // Joins/seals/freezes, stops the graph, then attempts completion. A temporary
    // snapshot collision leaves stopping; poll/retryCompletion can finish it.
    bool stop();
    // Call before an owner-observed input/rate/context change. Admitted originals
    // remain incomplete and retained; preparation is cancelled without a take.
    // An already collected pending result and its diagnostic remain unchanged.
    bool interrupt();
    bool retryCompletion();
    // Explicitly closes any active attempt and cancels its exact ticket before
    // releasing originals. A cancellation/cleanup failure preserves ownership.
    bool discardPending();

    RecordingStatus status() const noexcept;
    // Null until collection succeeds; a nonnull reference remains valid through
    // failed retries and ends at successful completion/discard or destruction.
    const PendingTake* pending() const noexcept;
    const ApplicationStatus& applicationStatus() const noexcept;
    std::exception_ptr exception() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
