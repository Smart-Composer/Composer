#include "RecordingDuration.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace composer::app
{
CompletionDuration recordingDuration(const std::optional<CaptureClockSnapshot>& snapshot,
                                    const CaptureMapping& mapping, const PendingTake& take) noexcept
{
    if (!take.isComplete()) return CompletionError::incompleteCapture;
    if (!snapshot) return CompletionError::unavailableSnapshot;
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (mapping.generation == 0 || mapping.generation == std::numeric_limits<std::uint64_t>::max()
        || mapping.proofSerial == 0 || !std::isfinite(mapping.sampleRate) || mapping.sampleRate <= 0.0
        || mapping.takeStartEditSample < 0
        || mapping.streamToEditOffset < minimum + mapping.takeStartEditSample)
        return CompletionError::invalidMapping;
    if (!snapshot->valid || snapshot->generation != mapping.generation
        || snapshot->serial < mapping.proofSerial || !std::isfinite(snapshot->rate)
        || snapshot->rate != mapping.sampleRate || !std::isfinite(snapshot->correction))
        return CompletionError::incompatibleSnapshot;
    const auto relativeOffset = mapping.streamToEditOffset - mapping.takeStartEditSample;
    if (snapshot->streamEnd < 0
        || (relativeOffset > 0 && snapshot->streamEnd > maximum - relativeOffset))
        return CompletionError::invalidEndpoint;
    const auto endpoint = snapshot->streamEnd + relativeOffset;
    if (endpoint < 0) return CompletionError::invalidEndpoint;
    const auto endpointSeconds = static_cast<double>(endpoint) / mapping.sampleRate;
    if (!std::isfinite(endpointSeconds)) return CompletionError::invalidEndpoint;
    if (endpointSeconds > project::maxProjectDurationSeconds) return CompletionError::durationExceeded;

    if (take.captured.capacity == 0 || take.captured.capacity > project::maxProjectEvents
        || take.captured.events.size() > take.captured.capacity)
        return CompletionError::invalidCapture;
    double latest = 0.0;
    for (const auto& event : take.captured.events)
    {
        if (!std::isfinite(event.timeSeconds) || event.timeSeconds < latest)
            return CompletionError::invalidCapture;
        if (event.timeSeconds > project::maxProjectDurationSeconds) return CompletionError::durationExceeded;
        latest = event.timeSeconds;
    }
    return std::max(endpointSeconds, latest);
}
}
