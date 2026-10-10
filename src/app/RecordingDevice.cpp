#include "RecordingDevice.h"

#include <stdexcept>
#include <utility>

namespace composer::app
{
namespace
{
void requireMessageThread()
{
    const auto* messages = juce::MessageManager::getInstanceWithoutCreating();
    if (messages == nullptr || !messages->isThisTheMessageThread())
        throw std::logic_error("MIDI selection requires the message thread");
}

juce::String physicalID(const juce::String& identifier)
{
    return "midiin_" + juce::String::toHexString(identifier.hashCode());
}

juce::String virtualID(const juce::String& name)
{
    return "vmidiin_" + juce::String::toHexString(name.hashCode());
}
}

struct RecordingDevice::Impl
{
    ApplicationProject& project;
    tracktion::Engine& engine;
    tracktion::DeviceManager& devices;
    RecordingDeviceStatus status;
    std::exception_ptr exception;
    juce::String identifier, name, deviceID, originalDefault, expectedFallback;
    std::shared_ptr<tracktion::PhysicalMidiInputDevice> physical;
    std::shared_ptr<tracktion::VirtualMidiInputDevice> dedicated;
    std::unique_ptr<tracktion::VirtualMidiInputDevice> removalDescriptor;
    bool priorEnabled = false, enablementOwned = false, virtualOwned = false;
    bool defaultPending = false, defaultOwned = false, restoringDefault = false;
    bool restoringEnablement = false;
    const std::uint64_t preparationTimeout;
    double preparingSince = 0.0;

    Impl(ApplicationProject& owner, std::uint64_t timeout)
        : project(owner), engine(owner.playbackEdit().engine), devices(engine.getDeviceManager()), preparationTimeout(timeout)
    {
        requireMessageThread();
        if (timeout == 0) throw std::invalid_argument("Invalid MIDI preparation timeout");
    }

    bool fail(RecordingDeviceFailure failure) noexcept { status.failure = failure; return false; }

    bool soleEdit() const
    {
        const auto edits = engine.getActiveEdits().getEdits();
        return edits.size() == 1 && edits[0] == &project.playbackEdit();
    }

    bool idle() const
    {
        auto& transport = project.playbackEdit().getTransport();
        const auto* context = transport.getCurrentPlaybackContext();
        return !project.session().isRecording() && !transport.isPlaying() && !transport.isRecording()
            && !transport.isStopping() && !transport.isRecordingStopping()
            && (context == nullptr || (!context->isPlaying() && !context->isPlayPending()));
    }

    bool controlPort(const tracktion::PhysicalMidiInputDevice& candidate) const
    {
        const auto& edit = project.playbackEdit();
        return candidate.isUsedForExternalControl()
            || (edit.isTimecodeSyncEnabled() && edit.getCurrentMidiTimecodeSource().get() == &candidate)
            || edit.getCurrentMidiMachineControlSource().get() == &candidate;
    }

    bool physicalCurrent() const
    {
        return physical && !physical->isEnabled() && !controlPort(*physical)
            && devices.findMidiInputDeviceForID(physical->getDeviceID()).get() == physical.get();
    }

    bool current() const
    {
        return soleEdit() && physicalCurrent() && dedicated && dedicated->isEnabled()
            && !dedicated->useAllInputs && dedicated->getMIDIInputSourceDevices().isEmpty()
            && dedicated->getMonitorMode() == tracktion::InputDevice::MonitorMode::on
            && devices.findMidiInputDeviceForID(deviceID).get() == dedicated.get();
    }

    juce::StringArray virtualNames() const
    {
        juce::StringArray result;
        result.addTokens(engine.getPropertyStorage().getProperty(tracktion::SettingID::virtualmididevices).toString(), ";", {});
        result.removeEmptyStrings();
        return result;
    }

    bool nameAvailable(const juce::String& candidate, const juce::String& id) const
    {
        if (devices.findMidiInputDeviceForID(id)) return false;
        for (const auto& existing : virtualNames())
            if (existing == candidate || existing == id || virtualID(existing) == id) return false;
        return engine.getPropertyStorage().getXmlPropertyItem(tracktion::SettingID::virtualmidiin, candidate) == nullptr;
    }

    juce::String configuredDefault() const
    {
        return engine.getPropertyStorage().getProperty(tracktion::SettingID::defaultMidiInDevice).toString();
    }

    void observeDefault()
    {
        if (!defaultPending) return;
        const auto configured = configuredDefault();
        const auto live = devices.getDefaultMidiInDeviceID();
        if (configured == originalDefault && live == originalDefault) return;
        defaultOwned = configured == expectedFallback && (live == originalDefault || live == expectedFallback);
        defaultPending = false;
        if (!defaultOwned) status.defaultRestorationSkipped = true;
    }

    bool release()
    {
        if (!idle()) return fail(RecordingDeviceFailure::busy);
        if (!soleEdit()) return fail(RecordingDeviceFailure::otherEdit);
        observeDefault();
        // Sole-edit ownership and this synchronous free establish the physical
        // closeDevice precondition: no edit input instance remains registered.
        project.stop();
        if (project.playbackEdit().getTransport().getCurrentPlaybackContext() != nullptr)
            return fail(RecordingDeviceFailure::busy);
        if (virtualOwned)
        {
            // A descriptor also permits cancellation before the rescan has
            // published an object. It has the exact fresh name/ID we created.
            devices.deleteVirtualMidiDevice(*removalDescriptor);
            // Virtual settings are saved by name; the pinned deletion removes
            // the ID key. Remove only this lease's fresh name-keyed settings.
            engine.getPropertyStorage().removePropertyItem(tracktion::SettingID::virtualmidiin, name);
            virtualOwned = false;
        }
        dedicated.reset();
        removalDescriptor.reset();
        if (enablementOwned)
        {
            const auto same = devices.findMidiInputDeviceForID(physical->getDeviceID()).get() == physical.get();
            if (same && !controlPort(*physical) && physical->isEnabled() == restoringEnablement)
            {
                restoringEnablement = true;
                physical->setEnabled(priorEnabled);
                // setEnabled may have changed memory before a prior save threw.
                // Retry persistence even when its value is already restored.
                physical->saveProps();
                // If cancellation precedes the first rescan, the manager's old
                // enabled-list snapshot already equals the restored value and
                // it can skip reopening. Restore this selected wrapper directly.
                if (physical->openDevice().isNotEmpty())
                {
                    status.state = RecordingDeviceState::failed;
                    return fail(RecordingDeviceFailure::openFailed);
                }
                restoringEnablement = false;
            }
            else status.restorationSkipped = true;
            if (defaultOwned)
            {
                const auto live = devices.getDefaultMidiInDeviceID();
                const auto configured = configuredDefault();
                if (same && !controlPort(*physical) && physical->isEnabled() && priorEnabled
                    && (configured == expectedFallback || (restoringDefault && configured == originalDefault))
                    && (live == originalDefault || live == expectedFallback))
                {
                    restoringDefault = true;
                    if (configured == originalDefault) devices.rescanMidiDeviceList();
                    else if (live != originalDefault) devices.setDefaultMidiInDevice(originalDefault);
                    else
                    {
                        // The automatic setter persists before the asynchronous
                        // live-ID update. Its public inverse is a no-op while the
                        // live ID still equals the original; undo only our exact
                        // observed pending property value in that interval.
                        engine.getPropertyStorage().setProperty(tracktion::SettingID::defaultMidiInDevice, originalDefault);
                        devices.rescanMidiDeviceList();
                    }
                    if (configuredDefault() != originalDefault)
                        throw std::runtime_error("Could not restore the selected MIDI default");
                }
                else status.defaultRestorationSkipped = true;
                defaultOwned = false;
                restoringDefault = false;
            }
            enablementOwned = false;
        }
        else if (physical && physical->isEnabled() != priorEnabled) status.restorationSkipped = true;
        physical.reset();
        identifier.clear(); name.clear(); deviceID.clear();
        originalDefault.clear(); expectedFallback.clear(); defaultPending = false;
        restoringEnablement = false;
        restoringDefault = false;
        status.state = RecordingDeviceState::unselected;
        return true;
    }

    bool caught() noexcept
    {
        exception = std::current_exception();
        status.failure = RecordingDeviceFailure::exception;
        status.state = RecordingDeviceState::failed;
        return false;
    }
};

RecordingDevice::RecordingDevice(ApplicationProject& project, std::uint64_t timeout)
    : impl_(std::make_unique<Impl>(project, timeout)) {}

RecordingDevice::~RecordingDevice()
{
    try
    {
        requireMessageThread();
        if (impl_->physical || impl_->virtualOwned) static_cast<void>(impl_->release());
    }
    catch (...)
    {
        // This lease registers no callback. Current devices remain manager-owned;
        // the detached descriptor has no instances. An unavailable restoration
        // must not turn ordinary shutdown into process termination. Call release
        // explicitly before destruction to observe and retry such failures.
    }
}

bool RecordingDevice::select(const juce::String& identifier)
{
    requireMessageThread();
    impl_->exception = {};
    impl_->status.failure = RecordingDeviceFailure::none;
    try
    {
        if (!impl_->idle()) return impl_->fail(RecordingDeviceFailure::busy);
        if (!impl_->soleEdit()) return impl_->fail(RecordingDeviceFailure::otherEdit);
        const auto inventory = juce::MidiInput::getAvailableDevices();
        const auto id = physicalID(identifier);
        unsigned identifierMatches = 0, hashMatches = 0;
        juce::String selectedName;
        for (const auto& candidate : inventory)
        {
            if (candidate.identifier == identifier) { ++identifierMatches; selectedName = candidate.name; }
            if (physicalID(candidate.identifier) == id) ++hashMatches;
        }
        if (identifier.isEmpty() || identifierMatches == 0) return impl_->fail(RecordingDeviceFailure::unknownInput);
        if (identifierMatches != 1 || hashMatches != 1) return impl_->fail(RecordingDeviceFailure::ambiguousInput);
        unsigned nameMatches = 0;
        for (const auto& candidate : inventory) if (candidate.name == selectedName) ++nameMatches;
        // The pinned physical settings store is keyed by name, not identifier.
        if (nameMatches != 1) return impl_->fail(RecordingDeviceFailure::ambiguousInput);
        auto physical = std::dynamic_pointer_cast<tracktion::PhysicalMidiInputDevice>(impl_->devices.findMidiInputDeviceForID(id));
        if (!physical || physical->getName() != selectedName) return impl_->fail(RecordingDeviceFailure::unavailablePhysical);
        if (impl_->controlPort(*physical)) return impl_->fail(RecordingDeviceFailure::externalControl);
        if (impl_->identifier == identifier && impl_->status.state == RecordingDeviceState::ready && impl_->current()) return true;
        if ((impl_->physical || impl_->virtualOwned) && !impl_->release()) return false;

        auto name = "Composer Recording " + juce::Uuid().toString();
        auto virtualId = virtualID(name);
        if (!impl_->nameAvailable(name, virtualId)) return impl_->fail(RecordingDeviceFailure::creationFailed);
        auto descriptor = std::make_unique<tracktion::VirtualMidiInputDevice>(impl_->engine, name,
            tracktion::InputDevice::virtualMidiDevice, virtualId, false);
        impl_->identifier = identifier;
        impl_->name = std::move(name);
        impl_->deviceID = std::move(virtualId);
        impl_->removalDescriptor = std::move(descriptor);
        impl_->physical = std::move(physical);
        impl_->priorEnabled = impl_->physical->isEnabled();
        impl_->status.restorationSkipped = false;
        impl_->status.defaultRestorationSkipped = false;
        impl_->originalDefault = impl_->devices.getDefaultMidiInDeviceID();
        impl_->defaultPending = impl_->priorEnabled && impl_->originalDefault == impl_->physical->getDeviceID();
        if (impl_->defaultPending)
        {
            // Match the pinned manager's fallback priority before changing the
            // selected port: All MIDI Ins, then its first enabled input.
            if (auto all = impl_->devices.findMidiInputDeviceForID("all_midi_in"); all && all->isEnabled())
                impl_->expectedFallback = all->getDeviceID();
            else
            {
                for (const auto& candidate : impl_->devices.getMidiInDevices())
                    if (candidate != impl_->physical && candidate->isEnabled())
                    { impl_->expectedFallback = candidate->getDeviceID(); break; }
                if (impl_->expectedFallback.isEmpty()) impl_->expectedFallback = impl_->deviceID;
            }
        }
        impl_->status.state = RecordingDeviceState::preparing;
        impl_->preparingSince = juce::Time::getMillisecondCounterHiRes();
        impl_->project.stop();
        if (impl_->project.playbackEdit().getTransport().getCurrentPlaybackContext() != nullptr)
            return impl_->fail(RecordingDeviceFailure::busy);
        impl_->enablementOwned = impl_->priorEnabled; // Retain only a change made by this lease.
        impl_->physical->setEnabled(false);
        impl_->physical->closeDevice();
        impl_->virtualOwned = true; // Creation may persist before throwing.
        if (impl_->devices.createVirtualMidiDevice(impl_->name).failed())
        {
            impl_->status.state = RecordingDeviceState::failed;
            return impl_->fail(RecordingDeviceFailure::creationFailed);
        }
        return true;
    }
    catch (...) { return impl_->caught(); }
}

bool RecordingDevice::poll()
{
    requireMessageThread();
    try
    {
        if (impl_->status.state == RecordingDeviceState::unselected) return true;
        if (impl_->status.state == RecordingDeviceState::failed) return false;
        impl_->observeDefault();
        if (!impl_->soleEdit() || !impl_->physicalCurrent())
        {
            impl_->status.state = RecordingDeviceState::failed;
            return impl_->fail(RecordingDeviceFailure::topologyChanged);
        }
        if (impl_->status.state == RecordingDeviceState::ready)
        {
            if (impl_->current()) return true;
            impl_->status.state = RecordingDeviceState::failed;
            return impl_->fail(RecordingDeviceFailure::topologyChanged);
        }
        const auto current = juce::Time::getMillisecondCounterHiRes();
        if (current < impl_->preparingSince || current - impl_->preparingSince >= static_cast<double>(impl_->preparationTimeout))
        {
            impl_->status.state = RecordingDeviceState::failed;
            return impl_->fail(RecordingDeviceFailure::preparationTimedOut);
        }
        if (!impl_->idle()) return impl_->fail(RecordingDeviceFailure::busy);
        auto candidate = std::dynamic_pointer_cast<tracktion::VirtualMidiInputDevice>(impl_->devices.findMidiInputDeviceForID(impl_->deviceID));
        if (!candidate) return true;
        if (candidate->getName() != impl_->name || candidate->useAllInputs)
        {
            impl_->status.state = RecordingDeviceState::failed;
            return impl_->fail(RecordingDeviceFailure::topologyChanged);
        }
        candidate->setMIDIInputSourceDevices({});
        candidate->setMonitorMode(tracktion::InputDevice::MonitorMode::on);
        candidate->setEnabled(true);
        impl_->dedicated = std::move(candidate);
        if (!impl_->current()) return impl_->fail(RecordingDeviceFailure::topologyChanged);
        impl_->status.state = RecordingDeviceState::ready;
        impl_->status.failure = RecordingDeviceFailure::none;
        return true;
    }
    catch (...) { return impl_->caught(); }
}

std::unique_ptr<juce::MidiInput> RecordingDevice::openStoppedInput()
{
    requireMessageThread();
    try
    {
        if (!impl_->idle()) { impl_->fail(RecordingDeviceFailure::busy); return {}; }
        if (!ready()) { impl_->fail(RecordingDeviceFailure::topologyChanged); return {}; }
        auto result = juce::MidiInput::openDevice(impl_->identifier, nullptr);
        impl_->status.failure = result ? RecordingDeviceFailure::none : RecordingDeviceFailure::openFailed;
        return result;
    }
    catch (...) { impl_->caught(); return {}; }
}

bool RecordingDevice::release()
{
    requireMessageThread();
    impl_->exception = {};
    impl_->status.failure = RecordingDeviceFailure::none;
    try { return impl_->release(); }
    catch (...) { return impl_->caught(); }
}

bool RecordingDevice::ready() const { return impl_->status.state == RecordingDeviceState::ready && impl_->current(); }
bool RecordingDevice::current() const { return impl_->current(); }
RecordingDeviceStatus RecordingDevice::status() const noexcept { return impl_->status; }
std::shared_ptr<tracktion::VirtualMidiInputDevice> RecordingDevice::dedicatedInput() const noexcept { return impl_->dedicated; }
std::exception_ptr RecordingDevice::exception() const noexcept { return impl_->exception; }
}
