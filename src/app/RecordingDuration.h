#pragma once

#include "CaptureClock.h"
#include "RecordingCapture.h"

#include <optional>
#include <variant>

namespace composer::app
{
enum class CompletionError
{
    incompleteCapture,
    unavailableSnapshot,
    invalidMapping,
    incompatibleSnapshot,
    invalidEndpoint,
    invalidCapture,
    durationExceeded
};

using CompletionDuration = std::variant<double, CompletionError>;

// Call off callback after every producer has joined and the take has been sealed.
// Read and retain the coherent final publisher snapshot before intentionally
// invalidating its generation or stopping/destroying its graph. This helper does
// not establish freshness, callback ownership, context identity or seal status.
// Failure retains the immutable originals; it never substitutes an event-only
// endpoint, clamps invalid time, repairs loss or validates a complete document.
CompletionDuration recordingDuration(const std::optional<CaptureClockSnapshot>&,
                                    const CaptureMapping&, const PendingTake&) noexcept;
}
