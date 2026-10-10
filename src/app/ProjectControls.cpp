#include "ProjectControls.h"
#include "ApplicationProject.h"

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace composer::app
{
namespace
{
juce::String text(std::string_view value)
{
    return juce::String::fromUTF8(value.data(), static_cast<int>(value.size()));
}

template<class Error>
juce::String errorText(const Error& error)
{
    if constexpr (std::is_same_v<Error, project::SessionError>)
        if (error.code == project::SessionErrorCode::unsavedChanges)
            return "Save the current project before creating or opening another project.";
    auto message = text(error.message);
    if (message.isEmpty()) message = "The operation could not be completed.";
    if constexpr (std::is_same_v<Error, project::ProjectError>
                  || std::is_same_v<Error, contracts::ContractError>)
        if (!error.field.empty()) message = text(error.field) + ": " + message;
    if constexpr (std::is_same_v<Error, project::ProjectFileError>)
        if (error.recoveryFile != juce::File{})
            message += " Recovery copy: " + error.recoveryFile.getFullPathName();
    return message;
}
}

class ProjectControls::Impl final : private juce::Timer, private juce::TableListBoxModel
{
public:
    Impl(ProjectControls& component, ApplicationProject& project)
        : view(component), owner(project), table("Original MIDI", this)
    {
        const std::array<std::pair<juce::TextButton*, const char*>, 12> buttons {{
            {&newButton, "New"}, {&openButton, "Open..."}, {&saveButton, "Save"},
            {&saveAsButton, "Save as..."}, {&undoButton, "Undo patch"}, {&redoButton, "Redo patch"},
            {&playButton, "Play"}, {&stopButton, "Stop"}, {&panicButton, "Panic"},
            {&recordButton, "Record"}, {&retryButton, "Retry take"}, {&discardButton, "Discard take"}
        }};
        for (const auto& [button, name] : buttons)
        {
            button->setButtonText(name);
            button->setName(name);
            view.addAndMakeVisible(*button);
        }
        newButton.onClick = [this] { createNew(); };
        openButton.onClick = [this] { chooseFile(false); };
        saveButton.onClick = [this] { save(false); };
        saveAsButton.onClick = [this] { save(true); };
        undoButton.onClick = [this] { if (editable()) showResult(owner.undoPatch(), "Patch undone."); };
        redoButton.onClick = [this] { if (editable()) showResult(owner.redoPatch(), "Patch redone."); };
        playButton.onClick = [this] { if (editable()) showResult(owner.playFromStart(), "Playing."); };
        stopButton.onClick = [this]
        {
            ++interactionEpoch;
            if (recordingBusy) invoke(view.onStopRecording);
            else { owner.stop(); setStatus("Stopped."); refresh(); }
        };
        panicButton.onClick = [this] { owner.panic(); setStatus("Notes released."); };
        recordButton.onClick = [this] { if (editable() && canRecord) invoke(view.onRecord); };
        retryButton.onClick = [this] { if (canRetry && !playing() && chooser == nullptr) invoke(view.onRetry); };
        discardButton.onClick = [this] { if (canDiscard && chooser == nullptr) invoke(view.onDiscard); };

        waveformLabel.setText("Waveform", juce::dontSendNotification);
        waveform.setName("Waveform");
        for (std::size_t index = 0; index < contracts::waveformDescriptors.size(); ++index)
            waveform.addItem(text(contracts::waveformDescriptors[index].displayName), static_cast<int>(index + 1));
        waveform.onChange = [this]
        {
            if (updating || !editable()) return;
            const auto selected = waveform.getSelectedId() - 1;
            if (selected < 0 || selected >= static_cast<int>(contracts::waveformDescriptors.size())) return;
            auto command = currentCommand();
            command.patch.waveform = contracts::waveformDescriptors[static_cast<std::size_t>(selected)].value;
            showResult(owner.apply(command), "Patch updated.");
        };
        view.addAndMakeVisible(waveformLabel);
        view.addAndMakeVisible(waveform);
        for (std::size_t index = 0; index < sliders.size(); ++index)
        {
            const auto& descriptor = contracts::parameterDescriptors[index + 1];
            auto& slider = sliders[index];
            slider.setName(text(descriptor.displayName));
            slider.setSliderStyle(juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 105, 24);
            slider.setRange(descriptor.minimum, descriptor.maximum);
            slider.setNumDecimalPlacesToDisplay(index == 5 ? 1 : 4);
            if (!descriptor.unit.empty()) slider.setTextValueSuffix(" " + text(descriptor.unit));
            slider.setDoubleClickReturnValue(true, descriptor.defaultValue);
            if (descriptor.id == "cutoff_hz") slider.setSkewFactorFromMidPoint(1000.0);
            labels[index].setText(text(descriptor.displayName), juce::dontSendNotification);
            slider.onDragStart = [this, index]
            {
                if (editable()) gestures[index] = Gesture{currentCommand(), interactionEpoch};
            };
            slider.onDragEnd = [this, index]
            {
                auto gesture = std::move(gestures[index]);
                gestures[index].reset();
                if (gesture && editable() && gesture->epoch == interactionEpoch)
                {
                    gesture->command.patch.*contracts::parameterDescriptors[index + 1].member = sliders[index].getValue();
                    showResult(owner.apply(gesture->command), "Patch updated.");
                }
                else refresh();
            };
            slider.onValueChange = [this, index]
            {
                if (updating || gestures[index] || !editable()) return;
                auto command = currentCommand();
                command.patch.*contracts::parameterDescriptors[index + 1].member = sliders[index].getValue();
                showResult(owner.apply(command), "Patch updated.");
            };
            view.addAndMakeVisible(labels[index]);
            view.addAndMakeVisible(slider);
        }
        for (auto* label : {&projectStatus, &recordingStatus, &status, &tableLabel})
        {
            label->setJustificationType(juce::Justification::centredLeft);
            view.addAndMakeVisible(*label);
        }
        status.setMinimumHorizontalScale(1.0f);
        status.setName("Project operation status");
        tableLabel.setText("Original MIDI performance (read only)", juce::dontSendNotification);
        table.getHeader().addColumn("Time (s)", 1, 150, 80, 250);
        table.getHeader().addColumn("Channel", 2, 85, 65, 120);
        table.getHeader().addColumn("Message / original bytes", 3, 480, 200, 1600);
        table.setRowHeight(24);
        table.setMultipleSelectionEnabled(false);
        view.addAndMakeVisible(table);
        refresh();
        startTimerHz(10);
    }

    ~Impl() override
    {
        stopTimer();
        chooser.reset();
        table.setModel(nullptr);
    }

    void setRecordingStatus(bool busy, bool start, bool retry, bool discard, juce::String message)
    {
        if (recordingBusy != busy) ++interactionEpoch;
        recordingBusy = busy;
        canRecord = start;
        canRetry = retry;
        canDiscard = discard;
        recordingStatus.setText(message, juce::dontSendNotification);
        recordingStatus.setTooltip(message);
        recordingStatus.setDescription(message);
        refresh();
    }

    void refresh()
    {
        const auto& snapshot = owner.session().patchSnapshot();
        const bool isPlaying = playing();
        const bool changedProject = snapshot.projectInstanceId != observedToken;
        if (changedProject || snapshot.revision != observedRevision || isPlaying != observedPlaying)
        {
            ++interactionEpoch;
            if (changedProject)
            {
                savedFile = {};
                table.deselectAllRows();
            }
            observedToken = snapshot.projectInstanceId;
            observedRevision = snapshot.revision;
            observedPlaying = isPlaying;
            table.updateContent();
            table.repaint();
        }
        const juce::ScopedValueSetter<bool> setting(updating, true);
        for (std::size_t index = 0; index < contracts::waveformDescriptors.size(); ++index)
            if (contracts::waveformDescriptors[index].value == snapshot.patch.waveform)
                waveform.setSelectedId(static_cast<int>(index + 1), juce::dontSendNotification);
        for (std::size_t index = 0; index < sliders.size(); ++index)
            if (!gestures[index])
                sliders[index].setValue(snapshot.patch.*contracts::parameterDescriptors[index + 1].member,
                                        juce::dontSendNotification);
        const auto ready = editable();
        for (auto* button : {&newButton, &openButton, &saveButton, &saveAsButton, &playButton}) button->setEnabled(ready);
        undoButton.setEnabled(ready && !owner.session().undoHistory().empty());
        redoButton.setEnabled(ready && !owner.session().redoHistory().empty());
        waveform.setEnabled(ready);
        for (auto& slider : sliders) slider.setEnabled(ready);
        recordButton.setEnabled(ready && canRecord && static_cast<bool>(view.onRecord));
        retryButton.setEnabled(canRetry && !isPlaying && chooser == nullptr && static_cast<bool>(view.onRetry));
        discardButton.setEnabled(canDiscard && chooser == nullptr && static_cast<bool>(view.onDiscard));
        stopButton.setEnabled(isPlaying || (recordingBusy && static_cast<bool>(view.onStopRecording)));
        panicButton.setEnabled(true);
        const auto& document = owner.session().document();
        projectStatus.setText((savedFile == juce::File{} ? juce::String("Untitled") : savedFile.getFileName())
            + (owner.session().isDirty() ? " - Unsaved changes" : " - No unsaved changes")
            + " | " + juce::String(static_cast<juce::int64>(document.events.size())) + " MIDI events | "
            + juce::String(document.durationSeconds, 3) + " s", juce::dontSendNotification);
    }

    void resized()
    {
        auto area = view.getLocalBounds().reduced(12);
        projectStatus.setBounds(area.removeFromTop(26));
        auto files = area.removeFromTop(32);
        for (auto* button : {&newButton, &openButton, &saveButton, &saveAsButton, &undoButton, &redoButton})
            button->setBounds(files.removeFromLeft(100).reduced(2));
        auto transport = area.removeFromTop(32);
        for (auto* button : {&playButton, &stopButton, &panicButton, &recordButton, &retryButton, &discardButton})
            button->setBounds(transport.removeFromLeft(100).reduced(2));
        recordingStatus.setBounds(area.removeFromTop(26));
        status.setBounds(area.removeFromTop(42));
        area.removeFromTop(4);
        for (std::size_t row = 0; row < 4; ++row)
        {
            auto line = area.removeFromTop(38);
            const auto width = line.getWidth() / 2;
            for (std::size_t column = 0; column < 2; ++column)
            {
                const auto index = row * 2 + column;
                auto cell = line.removeFromLeft(width).reduced(4, 2);
                if (index == 0)
                {
                    waveformLabel.setBounds(cell.removeFromLeft(75));
                    waveform.setBounds(cell);
                }
                else
                {
                    labels[index - 1].setBounds(cell.removeFromLeft(75));
                    sliders[index - 1].setBounds(cell);
                }
            }
        }
        tableLabel.setBounds(area.removeFromTop(26));
        table.setBounds(area);
    }

private:
    struct Gesture { contracts::ProjectCommand command; std::uint64_t epoch; };
    bool playing() const
    {
        const auto& transport = owner.playbackEdit().getTransport();
        return transport.isPlaying() || transport.isStopping() || transport.isRecordingStopping()
            || owner.playbackEdit().isRendering();
    }
    bool editable() const { return !recordingBusy && !owner.session().isRecording() && !playing() && chooser == nullptr; }
    contracts::ProjectCommand currentCommand() const
    {
        const auto& snapshot = owner.session().patchSnapshot();
        return {snapshot.projectInstanceId, snapshot.revision, snapshot.patch};
    }
    void timerCallback() override { refresh(); }
    void setStatus(const juce::String& message)
    {
        status.setText(message, juce::dontSendNotification);
        status.setTooltip(message);
        status.setDescription(message);
    }
    void invoke(const std::function<void()>& action)
    {
        const auto callback = action;
        if (!callback) return;
        ++interactionEpoch;
        juce::Component::SafePointer<ProjectControls> safe(&view);
        callback();
        if (safe != nullptr) safe->refresh();
    }
    template<class T>
    bool showResult(const ApplicationResult<T>& result, const juce::String& success)
    {
        bool ok = false;
        const auto message = std::visit([&](const auto& value) -> juce::String
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, T>)
            {
                ok = true;
                if constexpr (std::is_same_v<T, project::SaveReceipt>)
                    if (value.retainedPreviousVersion != juce::File{})
                        return success + " Previous version retained at " + value.retainedPreviousVersion.getFullPathName();
                return success;
            }
            else return errorText(value);
        }, result);
        setStatus(message);
        refresh();
        return ok;
    }
    void createNew()
    {
        if (!editable()) return;
        showResult(owner.newProject(), "New project.");
    }
    void save(bool saveAs)
    {
        refresh();
        if (!editable()) return;
        if (saveAs || savedFile == juce::File{}) { chooseFile(true); return; }
        saveTo(savedFile);
    }
    void saveTo(const juce::File& file)
    {
        const auto result = owner.save(file);
        if (showResult(result, "Project saved."))
        {
            savedFile = std::get<project::SaveReceipt>(result).target;
            refresh();
        }
    }
    void chooseFile(bool saving)
    {
        refresh();
        if (!editable()) return;
        if (!saving && owner.session().isDirty())
        {
            setStatus("Save the current project before opening another project.");
            return;
        }
        const auto expected = owner.session().context();
        const auto epoch = interactionEpoch;
        chooser = std::make_unique<juce::FileChooser>(saving ? "Save Composer project" : "Open Composer project",
            savedFile, "*.composer", true, false, &view);
        const auto chooserFlags = saving ? (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting)
                                  : (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles);
        juce::Component::SafePointer<ProjectControls> safe(&view);
        chooser->launchAsync(chooserFlags, [safe, expected, epoch, saving](const juce::FileChooser& selected)
        {
            const auto file = selected.getResult();
            if (safe != nullptr && safe->impl != nullptr)
                safe->impl->finishFileChoice(file, expected, epoch, saving);
        });
        refresh();
    }
    void finishFileChoice(const juce::File& file, const project::ProjectContext& expected,
                          std::uint64_t epoch, bool saving)
    {
        chooser.reset();
        const auto context = owner.session().context();
        const bool current = context.projectInstanceId == expected.projectInstanceId && context.revision == expected.revision
            && interactionEpoch == epoch && editable();
        if (file == juce::File{}) { refresh(); return; }
        if (!current)
        {
            setStatus("The project or recording state changed. Choose the file again when stopped.");
            refresh();
            return;
        }
        if (saving) saveTo(file);
        else if (showResult(owner.open(file), "Project opened."))
        {
            // showResult refreshes the new identity first, clearing any old path.
            savedFile = file;
            refresh();
        }
    }
    int getNumRows() override { return static_cast<int>(owner.session().document().events.size()); }
    void paintRowBackground(juce::Graphics& graphics, int row, int width, int height, bool selected) override
    {
        graphics.fillAll(selected ? juce::Colour(0xff354c68) : (row % 2 == 0 ? juce::Colour(0xff252a32) : juce::Colour(0xff20252c)));
        juce::ignoreUnused(width, height);
    }
    void paintCell(juce::Graphics& graphics, int row, int column, int width, int height, bool) override
    {
        const auto& events = owner.session().document().events;
        if (row < 0 || static_cast<std::size_t>(row) >= events.size()) return;
        const auto& event = events[static_cast<std::size_t>(row)];
        juce::String value;
        if (column == 1) value = juce::String(event.timeSeconds, 9);
        else if (column == 2 && !event.bytes.empty()) value = juce::String(1 + (event.bytes[0] & 15));
        else if (column == 3)
        {
            const juce::MidiMessage message(event.bytes.data(), static_cast<int>(event.bytes.size()), event.timeSeconds);
            value = message.getDescription() + " [";
            for (std::size_t index = 0; index < event.bytes.size(); ++index)
            {
                if (index != 0) value += " ";
                value += juce::String::toHexString(event.bytes[index]).paddedLeft('0', 2);
            }
            value += "]";
        }
        graphics.setColour(juce::Colours::white);
        graphics.drawText(value, 6, 0, width - 12, height, juce::Justification::centredLeft, true);
    }

    ProjectControls& view;
    ApplicationProject& owner;
    juce::TextButton newButton, openButton, saveButton, saveAsButton, undoButton, redoButton;
    juce::TextButton playButton, stopButton, panicButton, recordButton, retryButton, discardButton;
    juce::Label projectStatus, recordingStatus, status, tableLabel, waveformLabel;
    juce::ComboBox waveform;
    std::array<juce::Slider, 7> sliders;
    std::array<juce::Label, 7> labels;
    std::array<std::optional<Gesture>, 7> gestures;
    juce::TableListBox table;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File savedFile;
    std::string observedToken;
    std::uint64_t observedRevision = 0, interactionEpoch = 0;
    bool observedPlaying = false, updating = false;
    bool recordingBusy = false, canRecord = false, canRetry = false, canDiscard = false;
};

ProjectControls::ProjectControls(ApplicationProject& owner) : impl(std::make_unique<Impl>(*this, owner)) {}
ProjectControls::~ProjectControls() { impl.reset(); }
void ProjectControls::refresh() { if (impl != nullptr) impl->refresh(); }
void ProjectControls::setRecordingStatus(bool busy, bool canStart, bool canRetry, bool canDiscard, juce::String status)
{
    impl->setRecordingStatus(busy, canStart, canRetry, canDiscard, std::move(status));
}
void ProjectControls::resized() { if (impl != nullptr) impl->resized(); }
}
