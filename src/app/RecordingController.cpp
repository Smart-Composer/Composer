#include "RecordingController.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace composer::app
{
namespace
{
void requireMessageThread()
{
    const auto* messages = juce::MessageManager::getInstanceWithoutCreating();
    if (messages == nullptr || !messages->isThisTheMessageThread())
        throw std::logic_error("Recording requires the message thread");
}

template<class T>
ApplicationStatus projectStatus(ApplicationResult<T> result)
{
    return std::visit([](auto&& value) -> ApplicationStatus {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, T>) return std::monostate{};
        else return std::forward<decltype(value)>(value);
    }, std::move(result));
}

std::optional<std::int64_t> samples(double seconds, double rate) noexcept
{
    const auto rounded = seconds * rate + 0.5;
    if (!std::isfinite(seconds) || seconds < 0.0 || !std::isfinite(rate) || rate <= 0.0
        || !std::isfinite(rounded) || rounded >= 9223372036854775808.0) return {};
    return static_cast<std::int64_t>(rounded);
}

bool reachesOrigin(const CaptureClockSnapshot& snapshot, const CaptureMapping& mapping) noexcept
{
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (mapping.takeStartEditSample < 0 || mapping.streamToEditOffset < minimum + mapping.takeStartEditSample
        || snapshot.streamEnd < 0) return false;
    const auto offset = mapping.streamToEditOffset - mapping.takeStartEditSample;
    if (offset > 0 && snapshot.streamEnd > maximum - offset) return false;
    return snapshot.streamEnd + offset >= 0;
}
}

struct RecordingController::Impl
{
    ApplicationProject& project;
    tracktion::Engine& engine;
    tracktion::DeviceManager& devices;
    const std::size_t capacity;
    const std::uint64_t freshness, preparationTimeout;
    const RecordingInput::NowMilliseconds now;
    RecordingStatus status;
    ApplicationStatus applicationStatus{std::monostate{}};
    std::exception_ptr exception;
    std::unique_ptr<project::RecordingTicket> ticket;
    std::unique_ptr<juce::MidiInput> stoppedInput;
    std::shared_ptr<tracktion::VirtualMidiInputDevice> input;
    tracktion::Edit* edit = nullptr;
    tracktion::AudioTrack* track = nullptr;
    tracktion::Plugin::Ptr witness;
    CaptureClock* clock = nullptr; // DeviceManager owns the exclusive processor slot.
    std::unique_ptr<CaptureCalibration> calibration;
    std::optional<std::uint64_t> preparationGeneration;
    const tracktion::EditPlaybackContext* preparingContext = nullptr;
    std::optional<CaptureMapping> mapping;
    std::optional<CaptureClockSnapshot> admissionBaseline, endpoint;
    std::unique_ptr<RecordingCapture> capture;
    std::unique_ptr<RecordingInput> recordingInput;
    const PendingTake* retained = nullptr;
    std::uint64_t preparingSince = 0, endpointSince = 0;
    bool admissionAttempted = false, endpointFrozen = false, waitingForEndpoint = false;
    bool cancelPreparation = false;

    Impl(ApplicationProject& owner, std::size_t eventCapacity, std::uint64_t freshnessLimit,
         std::uint64_t timeout, RecordingInput::NowMilliseconds getNow)
        : project(owner), engine(owner.playbackEdit().engine), devices(engine.getDeviceManager()),
          capacity(eventCapacity), freshness(freshnessLimit), preparationTimeout(timeout), now(getNow)
    {
        requireMessageThread();
        if (capacity == 0 || capacity > project::maxProjectEvents || freshness == 0
            || preparationTimeout == 0 || now == nullptr)
            throw std::invalid_argument("Invalid recording controller limits");
        // The pinned registry ignores an already registered xml type.
        engine.getPluginManager().createBuiltInType<CaptureClockWitness>();
    }

    void clearError() noexcept
    {
        status.failure = RecordingFailure::none;
        status.durationError.reset();
        exception = {};
        applicationStatus = std::monostate{};
    }

    bool fail(RecordingFailure failure) noexcept { status.failure = failure; return false; }

    bool inputCurrent() const
    {
        return input && input->isEnabled() && !input->useAllInputs
            && input->getMonitorMode() == tracktion::InputDevice::MonitorMode::on
            && input->getMIDIInputSourceDevices().isEmpty()
            && devices.findMidiInputDeviceForID(input->getDeviceID()).get() == input.get();
    }

    bool graphCurrent() const
    {
        return edit != nullptr && track != nullptr && &project.playbackEdit() == edit
            && &project.playbackTrack() == track && !edit->isRendering()
            && project.playbackSampleRate() == devices.getSampleRate()
            && edit->tempoSequence.getTempo(0)->getBpm() == project::projectTempoBpm;
    }

    bool routeCurrent() const
    {
        unsigned selectedInstances = 0, selectedTargets = 0;
        for (auto* candidate : edit->getAllInputDevices())
        {
            const auto selected = &candidate->getInputDevice() == input.get();
            if (selected) ++selectedInstances;
            for (auto* destination : candidate->destinations)
            {
                if (!selected || destination->targetID != track->itemID) return false;
                ++selectedTargets;
            }
        }
        return selectedInstances == 1 && selectedTargets == 1;
    }

    bool runningCurrent() const
    {
        if (!graphCurrent() || !inputCurrent() || !routeCurrent() || !mapping || !calibration) return false;
        const auto* context = edit->getTransport().getCurrentPlaybackContext();
        if (!calibration->isContextCurrent(context) || context->getSampleRate() != mapping->sampleRate) return false;
        const auto sync = context->getSyncPoint();
        if (!sync || sync->time != sync->unloopedTime || sync->referenceSamplePosition < 0) return false;
        const auto editSample = samples(sync->unloopedTime.inSeconds(), mapping->sampleRate);
        return editSample && *editSample - sync->referenceSamplePosition == mapping->streamToEditOffset;
    }

    bool routeInput()
    {
        tracktion::InputDeviceInstance* selected = nullptr;
        for (auto* candidate : edit->getAllInputDevices())
            if (&candidate->getInputDevice() == input.get()) selected = candidate;
        if (selected == nullptr) return false;
        for (auto* candidate : edit->getAllInputDevices())
        {
            juce::Array<tracktion::EditItemID> targets;
            for (auto* destination : candidate->destinations) targets.add(destination->targetID);
            for (auto target : targets)
                if (candidate != selected || target != track->itemID)
                    if (candidate->removeTarget(target, nullptr).failed())
                        throw std::runtime_error("Could not isolate the recording input");
        }
        bool assignedToTrack = false;
        for (auto* destination : selected->destinations)
            if (destination->targetID == track->itemID) assignedToTrack = true;
        if (!assignedToTrack)
        {
            const auto assigned = selected->setTarget(track->itemID, true, &edit->getUndoManager(), 0);
            if (!assigned || *assigned == nullptr) throw std::runtime_error("Could not route the recording input");
        }
        return routeCurrent();
    }

    bool restartPreparation()
    {
        calibration.reset();
        preparationGeneration.reset();
        preparingContext = nullptr;
        mapping.reset();
        admissionBaseline.reset();
        clock->invalidate();
        auto& transport = edit->getTransport();
        transport.stop(true, true, false);
        transport.setPosition(tracktion::TimePosition());
        transport.ensureContextAllocated(true);
        if (!routeInput()) return false;
        transport.play(false);
        const auto generation = clock->beginGeneration();
        if (!generation) throw std::runtime_error("Recording clock is unavailable");
        calibration = std::make_unique<CaptureCalibration>(*generation);
        preparationGeneration = generation;
        preparingContext = transport.getCurrentPlaybackContext();
        return true;
    }

    void detachGraph()
    {
        // Every producer is already joined and the capture is sealed here.
        if (clock != nullptr)
        {
            if (devices.getGlobalOutputAudioProcessor() != clock)
                throw std::logic_error("Recording lost its exclusive output processor");
            devices.setGlobalOutputAudioProcessor(nullptr);
            clock = nullptr;
        }
        project.stop();
        if (witness != nullptr)
        {
            witness->deleteFromParent();
            witness = nullptr;
        }
        edit = nullptr;
        track = nullptr;
        calibration.reset();
        preparationGeneration.reset();
        preparingContext = nullptr;
        stoppedInput.reset();
    }

    void releaseAttempt() noexcept
    {
        recordingInput.reset();
        capture.reset();
        retained = nullptr;
        ticket.reset();
        input.reset();
        mapping.reset();
        endpoint.reset();
        admissionBaseline.reset();
        admissionAttempted = endpointFrozen = waitingForEndpoint = cancelPreparation = false;
        status.state = RecordingState::idle;
    }

    bool cancelUnadmitted()
    {
        status.state = RecordingState::stopping;
        cancelPreparation = true;
        // Also used after an explicit discard request. Cancellation retries must
        // finish a potentially failed join before releasing graph dependencies.
        if (capture && !capture->sealed()) capture->interrupt(CaptureInterruption::ownerAbandoned);
        if (recordingInput) { recordingInput->stop(); recordingInput.reset(); }
        if (capture && !capture->sealed())
            capture->sealAfterJoin(now(), static_cast<unsigned>(CaptureInterruption::ownerAbandoned));
        detachGraph();
        if (ticket)
        {
            applicationStatus = project.cancelRecording(*ticket);
            if (!std::holds_alternative<std::monostate>(applicationStatus))
            {
                status.state = RecordingState::pendingCompletion;
                return fail(RecordingFailure::projectRejected);
            }
        }
        releaseAttempt();
        return true;
    }

    bool failPreparation(RecordingFailure failure)
    {
        status.failure = failure;
        cancelUnadmitted();
        return false;
    }

    bool finishTake()
    {
        status.state = RecordingState::stopping;
        if (!capture) return cancelUnadmitted();
        if (!capture->sealed())
        {
            if (!runningCurrent()) capture->interrupt(CaptureInterruption::generationChanged);
            capture->close();
            if (recordingInput) { recordingInput->stop(); recordingInput.reset(); }
            else capture->sealAfterJoin(now(), static_cast<unsigned>(CaptureInterruption::ownerAbandoned));
        }
        if (!endpointFrozen)
        {
            if (!runningCurrent()) capture->interrupt(CaptureInterruption::generationChanged);
            const auto current = now();
            if (waitingForEndpoint && (current < endpointSince || current - endpointSince >= freshness))
                endpoint.reset();
            else
            {
                if (!waitingForEndpoint) { endpointSince = current; waitingForEndpoint = true; }
                endpoint = clock->read();
                if (!endpoint) return fail(RecordingFailure::unavailableEndpoint);
            }
            // Null after the bounded retry deadline, or an invalid snapshot, is
            // frozen as a completion failure. It must never be replaced later.
            endpointFrozen = true;
        }
        detachGraph();
        status.state = RecordingState::pendingCompletion;
        return complete();
    }

    bool complete()
    {
        if (cancelPreparation) return cancelUnadmitted();
        if (!capture || !ticket || !mapping || !endpointFrozen)
            throw std::logic_error("No sealed recording is ready for completion");
        retained = &capture->collect();
        status.arrivalTimeAdjustments = retained->arrivalTimeAdjustments;
        status.preOriginEvents = retained->preOriginEvents;
        if (!retained->isComplete()) return fail(RecordingFailure::captureInterrupted);
        const auto duration = recordingDuration(endpoint, *mapping, *retained);
        if (const auto* error = std::get_if<CompletionError>(&duration))
        {
            status.durationError = *error;
            return fail(*error == CompletionError::unavailableSnapshot
                ? RecordingFailure::unavailableEndpoint : RecordingFailure::invalidDuration);
        }
        auto result = project.commitRecording(*ticket, retained->captured, std::get<double>(duration));
        if (const auto* outcome = std::get_if<contracts::EditOutcome>(&result))
        {
            status.completed = *outcome;
            applicationStatus = std::monostate{};
            status.failure = RecordingFailure::none;
            releaseAttempt();
            return true;
        }
        applicationStatus = projectStatus(std::move(result));
        return fail(RecordingFailure::projectRejected);
    }

    bool pollPreparing()
    {
        const auto current = now();
        if (current < preparingSince || current - preparingSince >= preparationTimeout)
            return failPreparation(RecordingFailure::preparationTimedOut);
        if (!graphCurrent() || !inputCurrent()) return failPreparation(RecordingFailure::contextChanged);
        auto* context = edit->getTransport().getCurrentPlaybackContext();
        if (!calibration || context == nullptr || context != preparingContext || !context->isPlaybackGraphAllocated()
            || (!context->isPlaying() && !context->isPlayPending()))
        {
            restartPreparation();
            return true;
        }
        const auto snapshot = clock->read();
        if (!snapshot) return true;
        if (!preparationGeneration || snapshot->generation != *preparationGeneration)
        {
            restartPreparation();
            return true;
        }
        if (!snapshot->valid) return true;
        if (!mapping)
        {
            mapping = calibration->poll(*clock, context);
            if (!mapping) return true;
        }
        if (!runningCurrent()) return failPreparation(RecordingFailure::contextChanged);
        const auto ready = clock->read();
        if (!ready) return true;
        if (!ready->valid || ready->generation != mapping->generation || ready->rate != mapping->sampleRate)
        {
            restartPreparation();
            return true;
        }
        if (!admissionBaseline)
        {
            if (ready->serial >= mapping->proofSerial && reachesOrigin(*ready, *mapping)) admissionBaseline = ready;
            return true;
        }
        // Correction belongs to a block's start; the publisher reports its end.
        // Require a later completed block beyond an end already covering origin.
        if (ready->serial <= admissionBaseline->serial || ready->streamEnd <= admissionBaseline->streamEnd
            || ready->graphSerial <= admissionBaseline->graphSerial || ready->graphEditEnd <= admissionBaseline->graphEditEnd)
            return true;
        if (!runningCurrent()) return failPreparation(RecordingFailure::contextChanged);
        capture = std::make_unique<RecordingCapture>(clock->signals(), *mapping, capacity, current, freshness);
        admissionAttempted = true;
        recordingInput = std::make_unique<RecordingInput>(std::move(stoppedInput), input, *capture, now);
        status.state = RecordingState::recording;
        return true;
    }

    bool caught() noexcept
    {
        exception = std::current_exception();
        status.failure = RecordingFailure::exception;
        if (admissionAttempted)
        {
            if (capture && !capture->sealed()) capture->interrupt(CaptureInterruption::ownerAbandoned);
            status.state = RecordingState::stopping;
        }
        else if (ticket || clock || witness != nullptr) { cancelPreparation = true; status.state = RecordingState::stopping; }
        else { stoppedInput.reset(); input.reset(); status.state = RecordingState::idle; }
        return false;
    }
};

RecordingController::RecordingController(ApplicationProject& project, std::size_t capacity,
                                         std::uint64_t freshness, std::uint64_t timeout,
                                         RecordingInput::NowMilliseconds now)
    : impl_(std::make_unique<Impl>(project, capacity, freshness, timeout, now)) {}

RecordingController::~RecordingController()
{
    try
    {
        requireMessageThread();
        if (impl_->capture && !impl_->capture->sealed()) impl_->capture->interrupt(CaptureInterruption::ownerAbandoned);
        if (impl_->recordingInput) { impl_->recordingInput->stop(); impl_->recordingInput.reset(); }
        if (impl_->capture && !impl_->capture->sealed())
            impl_->capture->sealAfterJoin(impl_->now(), static_cast<unsigned>(CaptureInterruption::ownerAbandoned));
        if (impl_->ticket || impl_->clock || impl_->witness != nullptr)
        {
            impl_->detachGraph();
            if (!impl_->admissionAttempted && impl_->ticket)
                impl_->applicationStatus = impl_->project.cancelRecording(*impl_->ticket);
        }
    }
    catch (...) { std::terminate(); }
}

bool RecordingController::start(std::unique_ptr<juce::MidiInput> stopped,
                                std::shared_ptr<tracktion::VirtualMidiInputDevice> destination)
{
    requireMessageThread();
    if (impl_->status.state != RecordingState::idle) return impl_->fail(RecordingFailure::busy);
    impl_->clearError();
    impl_->status.completed.reset();
    impl_->status.arrivalTimeAdjustments = impl_->status.preOriginEvents = 0;
    try
    {
        impl_->input = std::move(destination);
        impl_->stoppedInput = std::move(stopped);
        if (!impl_->stoppedInput || !impl_->inputCurrent())
        {
            impl_->input.reset(); impl_->stoppedInput.reset();
            return impl_->fail(RecordingFailure::invalidInput);
        }
        if (impl_->devices.getGlobalOutputAudioProcessor() != nullptr)
        {
            impl_->input.reset(); impl_->stoppedInput.reset();
            return impl_->fail(RecordingFailure::unavailablePublisher);
        }
        auto result = impl_->project.beginRecording();
        if (auto* ticket = std::get_if<std::unique_ptr<project::RecordingTicket>>(&result)) impl_->ticket = std::move(*ticket);
        else
        {
            impl_->applicationStatus = projectStatus(std::move(result));
            impl_->input.reset(); impl_->stoppedInput.reset();
            return impl_->fail(RecordingFailure::projectRejected);
        }
        impl_->status.state = RecordingState::preparing;
        impl_->preparingSince = impl_->now();
        impl_->edit = &impl_->project.playbackEdit();
        impl_->track = &impl_->project.playbackTrack();
        impl_->witness = impl_->edit->getPluginCache().createNewPlugin(CaptureClockWitness::xmlTypeName, {});
        auto* witness = dynamic_cast<CaptureClockWitness*>(impl_->witness.get());
        if (witness == nullptr) throw std::runtime_error("Could not create the recording witness");
        impl_->track->pluginList.insertPlugin(impl_->witness, impl_->track->pluginList.size(), nullptr);
        auto publisher = std::make_unique<CaptureClock>(impl_->devices, impl_->input, *witness);
        auto* installed = publisher.get();
        impl_->devices.setGlobalOutputAudioProcessor(std::move(publisher));
        impl_->clock = installed;
        return true;
    }
    catch (...) { return impl_->caught(); }
}

bool RecordingController::poll()
{
    requireMessageThread();
    try
    {
        if (impl_->status.state == RecordingState::preparing) return impl_->pollPreparing();
        if (impl_->status.state == RecordingState::stopping)
            return impl_->cancelPreparation ? impl_->cancelUnadmitted() : impl_->finishTake();
        if (impl_->status.state != RecordingState::recording) return impl_->status.state == RecordingState::idle;
        if (!impl_->runningCurrent()) impl_->capture->interrupt(CaptureInterruption::generationChanged);
        if (!impl_->capture->poll(impl_->now())) return impl_->finishTake();
        return true;
    }
    catch (...) { return impl_->caught(); }
}

bool RecordingController::stop()
{
    requireMessageThread();
    if (impl_->status.state == RecordingState::idle) return true;
    return retryCompletion();
}

bool RecordingController::retryCompletion()
{
    requireMessageThread();
    if (impl_->status.state == RecordingState::idle) return true;
    impl_->clearError();
    try
    {
        if (!impl_->admissionAttempted || impl_->cancelPreparation) return impl_->cancelUnadmitted();
        if (impl_->status.state == RecordingState::pendingCompletion) return impl_->complete();
        return impl_->finishTake();
    }
    catch (...) { return impl_->caught(); }
}

bool RecordingController::interrupt()
{
    requireMessageThread();
    if (impl_->status.state == RecordingState::idle) return true;
    if (impl_->status.state == RecordingState::pendingCompletion) return false;
    impl_->clearError();
    try
    {
        if (!impl_->admissionAttempted) return impl_->cancelUnadmitted();
        if (impl_->capture) impl_->capture->interrupt(CaptureInterruption::generationChanged);
        return impl_->finishTake();
    }
    catch (...) { return impl_->caught(); }
}

bool RecordingController::discardPending()
{
    requireMessageThread();
    if (impl_->status.state == RecordingState::idle) return true;
    impl_->clearError();
    try
    {
        return impl_->cancelUnadmitted();
    }
    catch (...) { return impl_->caught(); }
}

RecordingStatus RecordingController::status() const noexcept { return impl_->status; }
const PendingTake* RecordingController::pending() const noexcept { return impl_->retained; }
const ApplicationStatus& RecordingController::applicationStatus() const noexcept { return impl_->applicationStatus; }
std::exception_ptr RecordingController::exception() const noexcept { return impl_->exception; }
}
