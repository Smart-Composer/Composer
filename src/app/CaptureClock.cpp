#include "CaptureClock.h"

#include <cmath>
#include <stdexcept>

namespace composer::app
{
namespace
{
constexpr auto invalidCorrection = std::numeric_limits<double>::quiet_NaN();
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free
              && std::atomic<std::int64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

std::optional<std::int64_t> samples(double seconds, double rate) noexcept
{
    const auto value = seconds * rate + 0.5;
    if (!std::isfinite(seconds) || seconds < 0.0 || !std::isfinite(rate) || rate <= 0.0
        || !std::isfinite(value) || value >= 9223372036854775808.0)
        return {};
    return static_cast<std::int64_t>(value);
}

bool steadyContext(const tracktion::EditPlaybackContext* context)
{
    return context != nullptr && context->isPlaybackGraphAllocated() && context->isPlaying()
        && !context->isPlayPending() && !context->getPendingPositionChange() && !context->isLooping()
        && !context->isDragging();
}

bool admissionReady(const CaptureClockSnapshot& snapshot, std::int64_t offset, std::int64_t origin) noexcept
{
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (snapshot.correctionAnchor < 0 || origin < 0
        || (offset > 0 && snapshot.correctionAnchor > maximum - offset)) return false;
    const auto anchor = snapshot.correctionAnchor + offset;
    const auto margin = std::ceil(snapshot.rate * 0.001) + 1.0;
    if (!std::isfinite(margin) || margin <= 0.0 || margin >= 9223372036854775808.0
        || anchor < origin) return false;
    return anchor - origin >= static_cast<std::int64_t>(margin);
}
}

void CaptureClockWitness::applyToBuffer(const tracktion::PluginRenderContext& context)
{
    if (context.isPlaying)
        if (const auto end = samples(context.editTime.getEnd().inSeconds(), rate))
        {
            editEnd.store(*end);
            serial.fetch_add(1);
        }
}

CaptureClock::CaptureClock(tracktion::DeviceManager& manager,
                          std::shared_ptr<tracktion::MidiInputDevice> midiInput,
                          CaptureClockWitness& witness)
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      devices(manager), input(std::move(midiInput)), graph(&witness)
{
    if (input == nullptr) throw std::invalid_argument("A recording clock requires a MIDI input");
}

void CaptureClock::invalidateWriter() noexcept
{
    armed = false;
    valid.store(false);
    published.correction.store(invalidCorrection);
    const auto generation = published.generation.load();
    if (generation != std::numeric_limits<std::uint64_t>::max())
        published.generation.store(generation + 1);
}

void CaptureClock::prepareToPlay(double newRate, int)
{
    version.fetch_add(1);
    invalidateWriter();
    rate.store(newRate);
    prepared = std::isfinite(newRate) && newRate > 0.0;
    streamEnd.store(0);
    correctionAnchor.store(-1);
    lastEnd = 0;
    version.fetch_add(1);
}

void CaptureClock::releaseResources()
{
    version.fetch_add(1);
    invalidateWriter();
    prepared = false;
    version.fetch_add(1);
}

std::optional<std::uint64_t> CaptureClock::beginGeneration()
{
    JUCE_ASSERT_MESSAGE_THREAD
    // Membership lookup may lock. Device-list publication is on this thread;
    // keep the query outside the audio lock and the snapshot writer interval.
    const bool currentInput = input->isEnabled()
        && devices.findMidiInputDeviceForID(input->getDeviceID()).get() == input.get();
    const juce::ScopedLock lock(devices.deviceManager.getAudioCallbackLock());
    version.fetch_add(1);
    invalidateWriter();
    const auto generation = published.generation.load();
    armed = prepared && generation != std::numeric_limits<std::uint64_t>::max()
        && currentInput && input->isEnabled();
    lastEnd = streamEnd.load();
    version.fetch_add(1);
    return armed ? std::optional{generation} : std::nullopt;
}

void CaptureClock::invalidate()
{
    JUCE_ASSERT_MESSAGE_THREAD
    const juce::ScopedLock lock(devices.deviceManager.getAudioCallbackLock());
    version.fetch_add(1);
    invalidateWriter();
    version.fetch_add(1);
}

void CaptureClock::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    version.fetch_add(1);
    const auto end = samples(devices.getCurrentStreamTime(), rate.load());
    const auto adjustment = input->getAdjustSecs();
    const auto anchor = samples(devices.getCurrentStreamTime()
        - static_cast<double>(audio.getNumSamples()) / rate.load() + devices.getOutputLatencySeconds(), rate.load());
    if (armed && (!end || !anchor || !input->isEnabled() || !std::isfinite(adjustment) || *end <= lastEnd
                  || published.serial.load() == std::numeric_limits<std::uint64_t>::max()))
        invalidateWriter();
    const auto usable = prepared && armed && end.has_value();
    streamEnd.store(end.value_or(0));
    graphSerial.store(graph->serial.load());
    graphEditEnd.store(graph->editEnd.load());
    correctionAnchor.store(anchor.value_or(-1));
    copiedCorrection.store(adjustment);
    valid.store(usable);
    published.correction.store(usable ? adjustment : invalidCorrection);
    const auto serial = published.serial.load();
    if (serial != std::numeric_limits<std::uint64_t>::max()) published.serial.store(serial + 1);
    lastEnd = end.value_or(0);
    version.fetch_add(1);
}

std::optional<CaptureClockSnapshot> CaptureClock::read() const noexcept
{
    const auto before = version.load();
    if ((before & 1U) != 0) return {};
    CaptureClockSnapshot result{published.generation.load(), published.serial.load(), graphSerial.load(),
        streamEnd.load(), graphEditEnd.load(), rate.load(), copiedCorrection.load(), valid.load(), correctionAnchor.load()};
    if (before != version.load()) return {};
    return result;
}

bool CaptureCalibration::isContextCurrent(const tracktion::EditPlaybackContext* context) const noexcept
{
    return identity == context && steadyContext(context);
}

std::optional<CaptureMapping> CaptureCalibration::poll(
    const CaptureClock& publisher, const tracktion::EditPlaybackContext* context)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const auto snapshot = publisher.read();
    if (!snapshot || !snapshot->valid || snapshot->generation != expectedGeneration
        || !steadyContext(context) || context->getSampleRate() != snapshot->rate
        || (identity != nullptr && identity != context))
    {
        resetPoints();
        return {};
    }
    identity = context;
    if (pending)
    {
        if (snapshot->serial <= pending->fenceSerial) return {};
        if (snapshot->graphSerial <= pending->fenceGraphSerial || snapshot->graphEditEnd < pending->edit)
        {
            resetPoints();
            return {};
        }
        const auto offset = pending->edit - pending->reference;
        if (completed)
        {
            if (pending->reference <= completed->reference || pending->edit <= completed->edit
                || pending->edit - completed->edit != pending->reference - completed->reference
                || offset != completed->edit - completed->reference)
            {
                resetPoints();
                return {};
            }
            if (!admissionReady(*snapshot, offset, pending->edit)) return {};
            // Waiting for the input clock adds callbacks after the two-point
            // proof. A transport move in that interval must invalidate it.
            const auto latest = context->getSyncPoint();
            const auto latestEdit = latest ? samples(latest->unloopedTime.inSeconds(), snapshot->rate) : std::nullopt;
            if (!latest || !latestEdit || latest->time != latest->unloopedTime
                || latest->referenceSamplePosition < 0 || *latestEdit - latest->referenceSamplePosition != offset)
            {
                resetPoints();
                return {};
            }
            return CaptureMapping{expectedGeneration, snapshot->serial, snapshot->rate, offset, pending->edit};
        }
        completed = pending;
        pending.reset();
    }
    const auto sync = context->getSyncPoint();
    const auto after = publisher.read();
    const auto edit = sync ? samples(sync->unloopedTime.inSeconds(), snapshot->rate) : std::nullopt;
    if (!sync || !after || !after->valid || after->generation != expectedGeneration
        || sync->time != sync->unloopedTime || sync->referenceSamplePosition < 0 || !edit)
    {
        resetPoints();
        return {};
    }
    pending = Point{after->serial, after->graphSerial, sync->referenceSamplePosition, *edit};
    return {};
}
}
