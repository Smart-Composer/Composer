#include "RecordingCapture.h"

#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>

namespace composer::app
{
namespace
{
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free
              && std::atomic<bool>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free);

std::int64_t checkedOffset(const CaptureMapping& mapping, std::uint64_t freshness)
{
    if (mapping.generation == 0 || mapping.generation == std::numeric_limits<std::uint64_t>::max()
        || mapping.proofSerial == 0 || freshness == 0
        || !std::isfinite(mapping.sampleRate) || mapping.sampleRate <= 0.0
        || mapping.takeStartEditSample < 0
        || mapping.streamToEditOffset < std::numeric_limits<std::int64_t>::min() + mapping.takeStartEditSample)
        throw std::invalid_argument("Invalid recording capture mapping");
    return mapping.streamToEditOffset - mapping.takeStartEditSample;
}
}

RecordingCapture::RecordingCapture(const CaptureClockSignals& clock, CaptureMapping mapping,
                                   std::size_t capacity, std::uint64_t nowMs, std::uint64_t freshnessLimitMs)
    : clock_(clock), mapping_(mapping), freshnessLimit_(freshnessLimitMs),
      relativeOffset_(checkedOffset(mapping, freshnessLimitMs)), buffer_(capacity),
      lastSerial_(mapping.proofSerial), lastProgress_(nowMs), lastPoll_(nowMs)
{
    if (buffer_.start() != project::CaptureStartResult::started)
        throw std::logic_error("Could not prepare recording capture");
}

RecordingCapture::~RecordingCapture()
{
    close();
    if (!sealed_) interrupt(CaptureInterruption::ownerAbandoned);
}

void RecordingCapture::interrupt(CaptureInterruption reason) noexcept
{
    faults_.fetch_or(static_cast<unsigned>(reason));
    accepting_.store(false);
}

bool RecordingCapture::clockMatches() noexcept
{
    const auto before = clock_.generation.load();
    const auto correction = clock_.correction.load();
    const auto after = clock_.generation.load();
    if (before != mapping_.generation || after != mapping_.generation)
        interrupt(CaptureInterruption::generationChanged);
    if (!std::isfinite(correction)) interrupt(CaptureInterruption::invalidCorrection);
    return faults_.load() == 0;
}

bool RecordingCapture::open(std::uint64_t nowMs) noexcept
{
    if (retired_ || sealed_) return false;
    retired_ = true;
    if (!poll(nowMs)) return false;
    accepting_.store(true);
    // A generation change after this check is latched by callback admission or
    // the final owner poll, even when no MIDI arrives in the meantime.
    opened_ = clockMatches();
    return opened_;
}

bool RecordingCapture::poll(std::uint64_t nowMs) noexcept
{
    if (sealed_) return faults_.load() == 0;
    if (nowMs < lastPoll_ || nowMs - lastPoll_ >= freshnessLimit_)
        interrupt(CaptureInterruption::lateOwnerPoll);
    lastPoll_ = nowMs;
    clockMatches();
    const auto serial = clock_.serial.load();
    if (serial < lastSerial_) interrupt(CaptureInterruption::regressedSerial);
    else if (serial > lastSerial_)
    {
        lastSerial_ = serial;
        lastProgress_ = nowMs;
    }
    if (nowMs < lastProgress_ || nowMs - lastProgress_ >= freshnessLimit_)
        interrupt(CaptureInterruption::stalePublisher);
    return faults_.load() == 0;
}

void RecordingCapture::close() noexcept
{
    accepting_.store(false);
    retired_ = true;
}

void RecordingCapture::sealAfterJoin(std::uint64_t nowMs, unsigned extraFaults) noexcept
{
    if (sealed_) return;
    close();
    poll(nowMs);
    faults_.fetch_or(extraFaults | (opened_ ? 0u : static_cast<unsigned>(CaptureInterruption::neverAdmitted)));
    sealed_ = true;
}

std::optional<double> RecordingCapture::admit() noexcept
{
    if (!accepting_.load()) return {};
    const auto before = clock_.generation.load();
    const auto correction = clock_.correction.load();
    const auto after = clock_.generation.load();
    if (before != mapping_.generation || after != mapping_.generation)
    {
        interrupt(CaptureInterruption::generationChanged);
        return {};
    }
    if (!std::isfinite(correction))
    {
        interrupt(CaptureInterruption::invalidCorrection);
        return {};
    }
    return correction;
}

double RecordingCapture::relativeSeconds(double rawTimestamp, double correction, bool& preOrigin) const noexcept
{
    preOrigin = false;
    constexpr auto invalid = std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(rawTimestamp) || rawTimestamp < 0.0) return invalid;
    const auto stream = rawTimestamp + correction;
    if (!std::isfinite(stream) || stream < 0.0) return invalid;
    // Matches the pinned Tracktion positive-time rule, before a safe integer cast.
    const auto rounded = stream * mapping_.sampleRate + 0.5;
    if (!std::isfinite(rounded) || rounded < 0.0 || rounded >= 9223372036854775808.0) return invalid;
    const auto sample = static_cast<std::int64_t>(rounded);
    if (relativeOffset_ > 0 && sample > std::numeric_limits<std::int64_t>::max() - relativeOffset_)
        return invalid;
    const auto relative = sample + relativeOffset_;
    if (relative < 0)
    {
        preOrigin = true;
        return 0.0;
    }
    const auto seconds = static_cast<double>(relative) / mapping_.sampleRate;
    return !std::isfinite(seconds) || seconds > project::maxProjectDurationSeconds ? invalid : seconds;
}

void RecordingCapture::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) noexcept
{
    const auto correction = admit();
    if (!correction) return;
    const auto size = message.getRawDataSize();
    const auto bytes = size > 0
        ? std::span<const std::uint8_t>(message.getRawData(), static_cast<std::size_t>(size))
        : std::span<const std::uint8_t>();
    bool preOrigin = false;
    const auto seconds = relativeSeconds(message.getTimeStamp(), *correction, preOrigin);
    if (buffer_.submit(seconds, bytes) == project::CaptureSubmitResult::accepted && preOrigin)
        preOriginEvents_.fetch_add(1);
}

const PendingTake& RecordingCapture::collect()
{
    if (!sealed_) throw std::logic_error("Join and seal recording capture before collection");
    if (pending_) return *pending_;
    auto prepared = std::make_unique<PendingTake>();
    auto result = buffer_.finish(project::CaptureFinishOrder::arrival);
    if (!result) throw std::logic_error("Missing retained recording capture");
    // Correction is refreshed per block, and can decrease between callbacks.
    // Preserve accepted arrival order even when raw timestamps are equal or
    // regress. Project timestamps remain nondecreasing without reordering notes.
    double previous = 0.0;
    for (auto& event : result->events)
    {
        if (event.timeSeconds < previous)
        {
            event.timeSeconds = previous;
            ++prepared->arrivalTimeAdjustments;
        }
        else previous = event.timeSeconds;
    }
    prepared->captured.events.swap(result->events);
    prepared->captured.capacity = result->capacity;
    prepared->captured.ignoredSystemMessages = result->ignoredSystemMessages;
    prepared->captured.invalidTimestamps = result->invalidTimestamps;
    prepared->captured.invalidMessages = result->invalidMessages;
    prepared->captured.overflowEvents = result->overflowEvents;
    prepared->interruptionFaults = faults_.load();
    prepared->preOriginEvents = preOriginEvents_.load();
    pending_.swap(prepared);
    return *pending_;
}

std::uint64_t RecordingInput::currentMilliseconds() noexcept
{
    return static_cast<std::uint64_t>(juce::Time::getMillisecondCounterHiRes());
}

RecordingInput::RecordingInput(std::unique_ptr<juce::MidiInput> stoppedInput,
                               std::shared_ptr<tracktion::VirtualMidiInputDevice> destination,
                               RecordingCapture& capture, NowMilliseconds now)
    : input_(std::move(stoppedInput)), destination_(std::move(destination)), capture_(capture), now_(now)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (!input_ || !destination_ || !now_ || capture_.sealed() || capture_.accepting())
        throw std::invalid_argument("Recording input requires stopped input and prepared capture");
    try
    {
        // Registration can allocate and can fail after adding an internal entry.
        // All members exist before publication; rollback removes any such entry.
        input_->addCallback(*this);
        if (!capture_.open(now_())) throw std::runtime_error("Recording clock admission failed");
        input_->start();
    }
    catch (...)
    {
        try { finish(static_cast<unsigned>(CaptureInterruption::ownerAbandoned)); }
        catch (...) { std::terminate(); }
        throw;
    }
}

RecordingInput::~RecordingInput()
{
    try { finish(static_cast<unsigned>(CaptureInterruption::ownerAbandoned)); }
    catch (...) { std::terminate(); }
}

void RecordingInput::stop() { finish(0); }

void RecordingInput::finish(unsigned extraFaults)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (finished_) return;
    capture_.close();
    input_->stop();
    input_->removeCallback(*this);
    capture_.sealAfterJoin(now_(), extraFaults);
    input_.reset();
    finished_ = true;
}

void RecordingInput::handleIncomingMidiMessage(juce::MidiInput* input, const juce::MidiMessage& message) noexcept
{
    capture_.handleIncomingMidiMessage(input, message);
    try { destination_->handleIncomingMidiMessage(message, destination_->getMPESourceID()); }
    catch (...) { capture_.interrupt(CaptureInterruption::monitoringFailed); }
}
}
