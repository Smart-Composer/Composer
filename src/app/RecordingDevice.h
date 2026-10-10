#pragma once

#include "ApplicationProject.h"

#include <exception>
#include <cstdint>
#include <memory>

namespace composer::app
{
enum class RecordingDeviceState { unselected, preparing, ready, failed };
enum class RecordingDeviceFailure
{
    none, busy, unknownInput, ambiguousInput, unavailablePhysical, externalControl,
    otherEdit, topologyChanged, preparationTimedOut, creationFailed, openFailed, exception
};

struct RecordingDeviceStatus
{
    RecordingDeviceState state = RecordingDeviceState::unselected;
    RecordingDeviceFailure failure = RecordingDeviceFailure::none;
    bool restorationSkipped = false;
    bool defaultRestorationSkipped = false;
};

// Message-thread lease for one selected native port and one isolated virtual
// monitoring input. The project/engine outlive this object. Every returned native
// input must be destroyed or joined and released by its recording controller
// before lease release/destruction. Configuration is exclusive while held.
class RecordingDevice final
{
public:
    explicit RecordingDevice(ApplicationProject&, std::uint64_t preparationTimeoutMs = 5000);
    // Best-effort fallback only. Explicit release exposes cleanup/restoration
    // failures while retry is possible. Destruction never terminates for a lost
    // port, and never removes routes from a still-busy or independently owned edit.
    ~RecordingDevice();
    RecordingDevice(const RecordingDevice&) = delete;
    RecordingDevice& operator=(const RecordingDevice&) = delete;

    // Explicit user selection enumerates the current native inventory once and
    // verifies the identifier's unambiguous physical mapping. No background poll
    // enumerates hardware. External control ports are refused.
    bool select(const juce::String& identifier);
    // Reconcile asynchronous manager publication through the normal event loop.
    // No sleeping, nested dispatch, physical opening or active rerouting occurs.
    bool poll();
    // Requires readiness and an idle project. Returns an opened but STOPPED input
    // without callbacks. The caller immediately transfers it to the controller or
    // destroys it before changing/releasing the lease. Null preserves selection.
    std::unique_ptr<juce::MidiInput> openStoppedInput();
    // Refuses an active/pending transport or recording ticket. Restores only the
    // selected prior enablement while its exact manager identity still matches.
    // The prior default is restored only while the observed automatic fallback
    // remains unchanged; an independent default choice is never overwritten.
    // A disappeared/replaced port is not restored; status reports that omission.
    bool release();

    bool ready() const;
    bool current() const;
    RecordingDeviceStatus status() const noexcept;
    std::shared_ptr<tracktion::VirtualMidiInputDevice> dedicatedInput() const noexcept;
    std::exception_ptr exception() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
