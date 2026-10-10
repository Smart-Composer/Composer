#include <composer/project/MidiCaptureBuffer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

namespace composer::project
{
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "MIDI capture requires lock-free 64-bit atomics");

struct MidiCaptureBuffer::Slot
{
    double timeSeconds = 0.0;
    std::array<std::uint8_t, 3> bytes {};
    std::uint8_t size = 0;
};

namespace
{
std::size_t checkedCapacity(std::size_t capacity)
{
    if (capacity == 0 || capacity > maxProjectEvents)
        throw std::invalid_argument("MIDI capture capacity is outside the project event limit");

    return capacity;
}

struct WriterScope
{
    std::atomic<std::uint64_t>& writers;

    ~WriterScope() { writers.fetch_sub(1); }
};
}

bool CapturedMidi::isComplete() const noexcept
{
    return invalidTimestamps == 0 && invalidMessages == 0 && overflowEvents == 0;
}

MidiCaptureBuffer::MidiCaptureBuffer(std::size_t capacity)
    : capacity_(checkedCapacity(capacity)), slots_(std::make_unique<Slot[]>(capacity_))
{
}

MidiCaptureBuffer::~MidiCaptureBuffer() = default;

CaptureStartResult MidiCaptureBuffer::start() noexcept
{
    // A stopped take can still be pending if finish() failed to allocate.
    if (hasTake_)
        return CaptureStartResult::alreadyActive;

    if (nextEpoch_ == std::numeric_limits<std::uint64_t>::max())
        return CaptureStartResult::epochExhausted;

    nextSlot_.store(0);
    ignoredSystemMessages_.store(0);
    invalidTimestamps_.store(0);
    invalidMessages_.store(0);
    overflowEvents_.store(0);
    hasTake_ = true;
    activeEpoch_.store(++nextEpoch_);
    return CaptureStartResult::started;
}

CaptureSubmitResult MidiCaptureBuffer::submit(
    double relativeTimeSeconds, std::span<const std::uint8_t> message) noexcept
{
    const auto epoch = activeEpoch_.load();
    if (epoch == 0)
        return CaptureSubmitResult::inactive;

    activeWriters_.fetch_add(1);
    const WriterScope writer { activeWriters_ };

    // Recheck after joining the writer count. A producer paused before joining
    // cannot write the old take after finish(), or contaminate a later take.
    if (activeEpoch_.load() != epoch)
        return CaptureSubmitResult::inactive;

    if (!std::isfinite(relativeTimeSeconds) || relativeTimeSeconds < 0.0
        || relativeTimeSeconds > maxProjectDurationSeconds)
    {
        invalidTimestamps_.fetch_add(1);
        return CaptureSubmitResult::invalidTimestamp;
    }

    if (!message.empty() && message.front() >= 0xf0)
    {
        ignoredSystemMessages_.fetch_add(1);
        return CaptureSubmitResult::ignoredSystemMessage;
    }

    const auto status = message.empty() ? 0 : message.front();
    const auto kind = status & 0xf0;
    const auto expectedSize = (kind == 0xc0 || kind == 0xd0) ? 2u : 3u;
    if (status < 0x80 || status >= 0xf0 || message.size() != expectedSize
        || message[1] >= 0x80 || (expectedSize == 3 && message[2] >= 0x80))
    {
        invalidMessages_.fetch_add(1);
        return CaptureSubmitResult::invalidMessage;
    }

    const auto index = nextSlot_.fetch_add(1);
    if (index >= capacity_)
    {
        overflowEvents_.fetch_add(1);
        return CaptureSubmitResult::overflow;
    }

    auto& slot = slots_[static_cast<std::size_t>(index)];
    slot.timeSeconds = relativeTimeSeconds;
    slot.size = static_cast<std::uint8_t>(expectedSize);
    for (std::size_t i = 0; i < expectedSize; ++i)
        slot.bytes[i] = message[i];

    return CaptureSubmitResult::accepted;
}

std::optional<CapturedMidi> MidiCaptureBuffer::finish(CaptureFinishOrder order)
{
    if (!hasTake_)
        return std::nullopt;

    activeEpoch_.exchange(0);
    while (activeWriters_.load() != 0)
        std::this_thread::yield();

    CapturedMidi result;
    result.capacity = capacity_;
    result.ignoredSystemMessages = ignoredSystemMessages_.load();
    result.invalidTimestamps = invalidTimestamps_.load();
    result.invalidMessages = invalidMessages_.load();
    result.overflowEvents = overflowEvents_.load();
    const auto count = static_cast<std::size_t>(
        std::min(nextSlot_.load(), static_cast<std::uint64_t>(capacity_)));
    result.events.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& slot = slots_[i];
        result.events.push_back({ slot.timeSeconds,
                                  { slot.bytes.begin(), slot.bytes.begin() + slot.size } });
    }

    if (order == CaptureFinishOrder::chronological)
        std::stable_sort(result.events.begin(), result.events.end(),
                         [](const MidiEvent& left, const MidiEvent& right)
                         { return left.timeSeconds < right.timeSeconds; });
    hasTake_ = false;
    return result;
}
}
