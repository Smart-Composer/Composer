#include "RecordingWorkspace.h"
#include "ApplicationProject.h"
#include "ProjectControls.h"
#include "RecordingController.h"
#include "RecordingDevice.h"

#include <array>
#include <cmath>
#include <type_traits>
#include <utility>

namespace composer::app
{
namespace
{
double initialRate(tracktion::Engine& engine)
{
    const auto rate = engine.getDeviceManager().getSampleRate();
    return std::isfinite(rate) && rate > 0.0 ? rate : 48000.0;
}

juce::String applicationMessage(const ApplicationStatus& result)
{
    return std::visit([](const auto& value) -> juce::String {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::monostate>) return {};
        else return value.message.empty() ? "The project operation could not be completed." : juce::String(value.message);
    }, result);
}

juce::String recordingMessage(const RecordingController& controller)
{
    const auto status = controller.status();
    switch (status.failure)
    {
        case RecordingFailure::busy: return "Finish or discard the current take first.";
        case RecordingFailure::invalidInput: return "The selected MIDI input is no longer ready.";
        case RecordingFailure::unavailablePublisher: return "The recording clock is unavailable.";
        case RecordingFailure::preparationTimedOut: return "Recording did not start. Check the audio output and MIDI input, then try again.";
        case RecordingFailure::contextChanged: return "The playback or input setup changed before recording was ready.";
        case RecordingFailure::captureInterrupted: return "Recording was interrupted. The original events are retained; this take cannot replace the project.";
        case RecordingFailure::unavailableEndpoint: return "The recording endpoint is unavailable. The captured events are retained.";
        case RecordingFailure::invalidDuration: return "The take has invalid timing. The captured events are retained.";
        case RecordingFailure::projectRejected:
        {
            const auto detail = applicationMessage(controller.applicationStatus());
            return "The take is retained. " + detail;
        }
        case RecordingFailure::exception: return "The recording operation failed. Retry completion or explicitly discard the retained take.";
        case RecordingFailure::none: break;
    }
    switch (status.state)
    {
        case RecordingState::preparing: return "Preparing the instrument and recording clock...";
        case RecordingState::recording: return "Recording original MIDI. Stop to keep the take, or Discard take to cancel.";
        case RecordingState::stopping: return "Finishing the take...";
        case RecordingState::pendingCompletion: return "The take is retained. Retry completion or discard it.";
        case RecordingState::idle:
            if (!status.completed) return {};
            return status.arrivalTimeAdjustments != 0 || status.preOriginEvents != 0
                ? "Take recorded. Timing was adjusted to preserve MIDI message order; save the project to keep it."
                : "Take recorded. Save the project to keep it.";
    }
    return {};
}

juce::String deviceMessage(const RecordingDeviceStatus& status)
{
    switch (status.failure)
    {
        case RecordingDeviceFailure::none: break;
        case RecordingDeviceFailure::busy: return "Stop playback and finish the current take before changing MIDI input.";
        case RecordingDeviceFailure::unknownInput: return "That MIDI input is no longer available. Refresh the input list.";
        case RecordingDeviceFailure::ambiguousInput: return "The MIDI input cannot be identified uniquely.";
        case RecordingDeviceFailure::unavailablePhysical: return "The engine has not found that MIDI input yet. Refresh and try again.";
        case RecordingDeviceFailure::externalControl: return "That MIDI input is assigned to an external controller.";
        case RecordingDeviceFailure::otherEdit: return "Another project is using this engine's MIDI input.";
        case RecordingDeviceFailure::topologyChanged: return "The MIDI device setup changed. Select the input again when stopped.";
        case RecordingDeviceFailure::creationFailed: return "The MIDI monitoring input could not be prepared.";
        case RecordingDeviceFailure::preparationTimedOut: return "The MIDI input did not become ready. Refresh and select it again.";
        case RecordingDeviceFailure::openFailed: return "The MIDI input could not be opened. Check its connection and whether another application is using it.";
        case RecordingDeviceFailure::exception: return "The MIDI input operation failed. Stop and select the input again.";
    }
    if (status.restorationSkipped || status.defaultRestorationSkipped)
        return "A device setting changed independently and was left unchanged during cleanup.";
    switch (status.state)
    {
        case RecordingDeviceState::unselected: return "Refresh MIDI inputs, then select an instrument to record.";
        case RecordingDeviceState::preparing: return "Preparing the selected MIDI input...";
        case RecordingDeviceState::ready: return "MIDI input ready.";
        case RecordingDeviceState::failed: return "Select the MIDI input again when stopped.";
    }
    return {};
}

class AudioOutputWindow final : public juce::DocumentWindow
{
public:
    explicit AudioOutputWindow(juce::AudioDeviceManager& manager)
        : DocumentWindow("Audio output", juce::Colour(0xff25313a), DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(manager, 0, 0, 0, 2, false, false, true, false);
        selector->setSize(600, 450);
        setContentOwned(selector.release(), true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
    }
    void closeButtonPressed() override { setVisible(false); }
};
}

class RecordingWorkspace::Impl final : private juce::Timer
{
public:
    Impl(RecordingWorkspace& component, tracktion::Engine& owner)
        : view(component), engine(owner), project(engine, initialRate(engine)), device(project),
          controller(project, project::maxProjectEvents), controls(project), tooltips(&view, 650)
    {
        inputLabel.setText("MIDI input", juce::dontSendNotification);
        inputList.setTextWhenNothingSelected("Refresh to find MIDI inputs");
        inputList.setName("MIDI input");
        refreshButton.setButtonText("Refresh inputs");
        audioButton.setButtonText("Audio output...");
        refreshButton.onClick = [this] { refreshInputs(); };
        inputList.onChange = [this] { selectInput(); };
        audioButton.onClick = [this] { showAudioOutput(); };
        controls.onRecord = [this] { startRecording(); };
        controls.onStopRecording = [this]
        {
            localMessage.clear();
            if (controller.status().state != RecordingState::idle) controller.stop();
            else if (device.status().state == RecordingDeviceState::preparing) device.release();
            else project.stop();
            updateControls();
        };
        controls.onRetry = [this] { localMessage.clear(); controller.retryCompletion(); updateControls(); };
        controls.onDiscard = [this] { localMessage.clear(); controller.discardPending(); updateControls(); };
        for (auto* child : std::array<juce::Component*, 6>{&inputLabel, &inputList, &refreshButton, &audioButton, &deviceStatus, &controls})
            view.addAndMakeVisible(*child);
        deviceStatus.setMinimumHorizontalScale(1.0f);
        updateControls();
        startTimerHz(50);
    }

    ~Impl() override
    {
        stopTimer();
        closeDialog.close();
        audioOutput.reset();
        // Normal window closure is refused until the take is resolved. An
        // external application shutdown still has to join every callback.
        if (controller.status().state != RecordingState::idle) controller.discardPending();
        project.stop();
        device.release();
    }

    void resized()
    {
        auto area = view.getLocalBounds();
        auto devices = area.removeFromTop(40).reduced(12, 4);
        inputLabel.setBounds(devices.removeFromLeft(80));
        audioButton.setBounds(devices.removeFromRight(135).reduced(2));
        refreshButton.setBounds(devices.removeFromRight(125).reduced(2));
        inputList.setBounds(devices.reduced(2));
        deviceStatus.setBounds(area.removeFromTop(34).reduced(12, 0));
        controls.setBounds(area);
    }

    void requestClose(std::function<void()> close)
    {
        if (closeDialogOpen) return;
        if (controller.status().state != RecordingState::idle || project.session().isRecording())
        {
            localMessage = "Stop and save or discard the current take before closing.";
            updateControls();
            return;
        }
        if (!project.session().isDirty()) { completeClose(std::move(close)); return; }
        confirmClose("Unsaved project", "The project has unsaved changes.", "Close without saving",
            [this, close = std::move(close)]() mutable { completeClose(std::move(close)); });
    }

private:
    void confirmClose(const juce::String& title, const juce::String& message,
                      const juce::String& affirmative, std::function<void()> close)
    {
        const auto expected = project.session().context();
        closeDialogOpen = true;
        updateControls();
        juce::Component::SafePointer<RecordingWorkspace> safe(&view);
        closeDialog = juce::AlertWindow::showScopedAsync(juce::MessageBoxOptions{}
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(title).withMessage(message)
            .withButton(affirmative).withButton("Keep working")
            .withAssociatedComponent(&view),
            [safe, expected, close = std::move(close)](int result)
            {
                if (safe == nullptr || safe->impl == nullptr) return;
                auto& self = *safe->impl;
                self.closeDialogOpen = false;
                const auto current = self.project.session().context();
                if (result == 1 && current.projectInstanceId == expected.projectInstanceId
                    && current.revision == expected.revision && self.controller.status().state == RecordingState::idle
                    && !self.project.session().isRecording())
                    close();
                else self.updateControls();
            });
    }

    void completeClose(std::function<void()> close)
    {
        project.stop();
        if (device.release()) { close(); return; }
        localMessage = deviceMessage(device.status());
        updateControls();
        confirmClose("MIDI input cleanup", localMessage + " The previous MIDI settings could not be fully restored.",
            "Close anyway", std::move(close));
    }

    bool playing() const
    {
        const auto& edit = project.playbackEdit();
        const auto& transport = edit.getTransport();
        return transport.isPlaying() || transport.isStopping() || transport.isRecordingStopping() || edit.isRendering();
    }
    bool configurationAllowed() const
    {
        return controller.status().state == RecordingState::idle && !project.session().isRecording() && !playing() && !closeDialogOpen;
    }
    void setDeviceMessage(const juce::String& message)
    {
        deviceStatus.setText(message, juce::dontSendNotification);
        deviceStatus.setTooltip(message);
        deviceStatus.setDescription(message);
    }
    void refreshInputs()
    {
        if (!configurationAllowed()) return;
        localMessage.clear();
        inventory = juce::MidiInput::getAvailableDevices();
        inputList.clear(juce::dontSendNotification);
        inputList.addItem("No MIDI input", 1);
        int selected = 1;
        for (int index = 0; index < inventory.size(); ++index)
        {
            inputList.addItem(inventory[index].name, index + 2);
            if (inventory[index].identifier == selectedIdentifier) selected = index + 2;
        }
        inputList.setSelectedId(selected, juce::dontSendNotification);
        if (selected == 1 && selectedIdentifier.isNotEmpty())
        {
            if (device.release()) selectedIdentifier.clear();
            else localMessage = "The previous MIDI input could not be released. Stop and select it again.";
        }
        engine.getDeviceManager().rescanMidiDeviceList();
        updateControls();
    }
    void selectInput()
    {
        if (!configurationAllowed()) { restoreVisibleSelection(); return; }
        localMessage.clear();
        const auto index = inputList.getSelectedId() - 2;
        if (index < 0)
        {
            if (device.release()) selectedIdentifier.clear();
        }
        else if (index < inventory.size())
        {
            if (device.select(inventory[index].identifier)) selectedIdentifier = inventory[index].identifier;
        }
        restoreVisibleSelection();
        updateControls();
    }
    void restoreVisibleSelection()
    {
        int selected = 1;
        for (int item = 0; item < inventory.size(); ++item)
            if (inventory[item].identifier == selectedIdentifier) selected = item + 2;
        inputList.setSelectedId(selected, juce::dontSendNotification);
    }
    bool selectionMatches() const
    {
        const auto index = inputList.getSelectedId() - 2;
        return index >= 0 && index < inventory.size() && selectedIdentifier.isNotEmpty()
            && inventory[index].identifier == selectedIdentifier;
    }
    void startRecording()
    {
        if (!configurationAllowed() || !selectionMatches() || !device.ready() || (audioOutput && audioOutput->isVisible())) return;
        localMessage.clear();
        try { recordingProjectId = project.session().context().projectInstanceId; }
        catch (...)
        {
            localMessage = "The project identity could not be read. Try recording again.";
            updateControls();
            return;
        }
        auto input = device.openStoppedInput();
        if (input != nullptr) controller.start(std::move(input), device.dedicatedInput());
        updateControls();
    }
    void showAudioOutput()
    {
        if (!configurationAllowed()) return;
        localMessage.clear();
        if (!audioOutput) audioOutput = std::make_unique<AudioOutputWindow>(engine.getDeviceManager().deviceManager);
        audioOutput->setVisible(true);
        audioOutput->toFront(true);
        updateControls();
    }
    void timerCallback() override
    {
        const auto state = controller.status().state;
        if (state == RecordingState::preparing || state == RecordingState::recording)
        {
            if (!device.current()) controller.interrupt();
            else controller.poll();
        }
        else if (state == RecordingState::stopping) controller.poll();
        else if (state == RecordingState::idle)
        {
            device.poll();
            const auto rate = engine.getDeviceManager().getSampleRate();
            if (std::isfinite(rate) && rate > 0.0 && rate != project.playbackSampleRate())
            {
                project.stop();
                const auto result = project.setSampleRate(rate);
                if (!std::holds_alternative<std::monostate>(result)) localMessage = applicationMessage(result);
            }
        }
        updateControls();
    }
    void updateControls()
    {
        const auto record = controller.status();
        const auto selected = device.status();
        const bool audioOpen = audioOutput && audioOutput->isVisible();
        const bool busy = record.state != RecordingState::idle || selected.state == RecordingDeviceState::preparing || audioOpen || closeDialogOpen;
        const bool canConfigure = configurationAllowed() && selected.state != RecordingDeviceState::preparing && !audioOpen;
        inputList.setEnabled(canConfigure);
        refreshButton.setEnabled(canConfigure);
        audioButton.setEnabled(configurationAllowed() && selected.state != RecordingDeviceState::preparing);
        const auto& document = project.session().document();
        const bool firstTake = document.events.empty() && document.durationSeconds == 0.0;
        auto message = recordingMessage(controller);
        if (record.completed && (firstTake || project.session().context().projectInstanceId != recordingProjectId))
            message.clear();
        if (message.isEmpty()) message = firstTake ? "Stop to keep a recording. Original MIDI is preserved without quantization."
                                                   : "Save this performance before creating a new project for another take.";
        controls.setRecordingStatus(busy, !busy && firstTake && selectionMatches() && device.ready(),
            record.state == RecordingState::pendingCompletion, record.state != RecordingState::idle, message);
        setDeviceMessage(localMessage.isEmpty() ? deviceMessage(selected) : localMessage);
    }

    RecordingWorkspace& view;
    tracktion::Engine& engine;
    ApplicationProject project;
    RecordingDevice device;
    RecordingController controller;
    ProjectControls controls;
    juce::TooltipWindow tooltips;
    juce::Label inputLabel, deviceStatus;
    juce::ComboBox inputList;
    juce::TextButton refreshButton, audioButton;
    juce::Array<juce::MidiDeviceInfo> inventory;
    juce::String selectedIdentifier, localMessage;
    std::string recordingProjectId;
    std::unique_ptr<AudioOutputWindow> audioOutput;
    juce::ScopedMessageBox closeDialog;
    bool closeDialogOpen = false;
};

RecordingWorkspace::RecordingWorkspace(tracktion::Engine& engine) : impl(std::make_unique<Impl>(*this, engine)) {}
RecordingWorkspace::~RecordingWorkspace() { impl.reset(); }
void RecordingWorkspace::resized() { if (impl != nullptr) impl->resized(); }
void RecordingWorkspace::requestClose(std::function<void()> close) { impl->requestClose(std::move(close)); }
}
