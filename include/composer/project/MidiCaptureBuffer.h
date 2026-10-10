#pragma once

#include <composer/project/ProjectDocument.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace composer::project
{
enum class CaptureStartResult
{
    started,
    alreadyActive,
    epochExhausted
};

enum class CaptureSubmitResult
{
    accepted,
    inactive,
    ignoredSystemMessage,
    invalidTimestamp,
    invalidMessage,
    overflow
};

enum class CaptureFinishOrder { chronological, arrival };

struct CapturedMidi
{
    std::vector<MidiEvent> events;
    std::size_t capacity = 0;
    std::uint64_t ignoredSystemMessages = 0;
    std::uint64_t invalidTimestamps = 0;
    std::uint64_t invalidMessages = 0;
    std::uint64_t overflowEvents = 0;

    // System messages are outside the channel-message document format. Ignoring
    // them does not make that channel-message capture incomplete.
    bool isComplete() const noexcept;
};

// Own on one control thread; only submit() may run on other threads. Prepare all
// storage before attaching callbacks. start() and finish() must not overlap each
// other and never belong in audio/MIDI callbacks. Detach and join every producer
// before destroying this object. submit() is safe for multiple producers.
// The epoch guard covers work inside submit(), not timestamps computed before
// entry. Detach and quiesce callbacks around take/origin changes; an invocation
// first entering submit() after a new start belongs to the new take.
class MidiCaptureBuffer final
{
public:
    // Throws std::invalid_argument for zero or capacity > maxProjectEvents.
    explicit MidiCaptureBuffer(std::size_t capacity);
    ~MidiCaptureBuffer();

    MidiCaptureBuffer(const MidiCaptureBuffer&) = delete;
    MidiCaptureBuffer& operator=(const MidiCaptureBuffer&) = delete;
    MidiCaptureBuffer(MidiCaptureBuffer&&) = delete;
    MidiCaptureBuffer& operator=(MidiCaptureBuffer&&) = delete;

    CaptureStartResult start() noexcept;

    // Full MIDI 1 channel messages only: no running status. Relative timestamps
    // must be finite and within [0, maxProjectDurationSeconds]. The callback path
    // performs no allocation, waiting, locks, I/O, or unbounded retry loops.
    // The caller owns timestamp-origin consistency across take boundaries.
    CaptureSubmitResult submit(double relativeTimeSeconds,
                               std::span<const std::uint8_t> message) noexcept;

    // Returns nullopt if inactive. Stops accepting this take, waits for its
    // admitted writers, then allocates the result off the callback thread.
    // Chronological order is the default; equal timestamps retain accepted
    // submission order. Arrival order preserves slot reservation order, including
    // when producers overlap. Allocation failure leaves the stopped take
    // available for another finish() attempt, in either order.
    std::optional<CapturedMidi> finish(CaptureFinishOrder = CaptureFinishOrder::chronological);

private:
    struct Slot;

    const std::size_t capacity_;
    const std::unique_ptr<Slot[]> slots_;
    std::atomic<std::uint64_t> activeEpoch_ { 0 };
    std::atomic<std::uint64_t> activeWriters_ { 0 };
    std::atomic<std::uint64_t> nextSlot_ { 0 };
    std::atomic<std::uint64_t> ignoredSystemMessages_ { 0 };
    std::atomic<std::uint64_t> invalidTimestamps_ { 0 };
    std::atomic<std::uint64_t> invalidMessages_ { 0 };
    std::atomic<std::uint64_t> overflowEvents_ { 0 };
    std::uint64_t nextEpoch_ = 0;
    bool hasTake_ = false;
};
}
