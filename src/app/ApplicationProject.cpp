#include "ApplicationProject.h"
#include "InstrumentAdapter.h"

#include <cmath>
#include <new>
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
        throw std::logic_error("Application projects require the message thread");
}

std::string freshToken() { return juce::Uuid().toString().toStdString(); }

project::ProjectSession initialSession()
{
    requireMessageThread();
    auto result = project::ProjectSession::create(freshToken());
    if (auto* session = std::get_if<project::ProjectSession>(&result))
        return std::move(*session);
    throw std::runtime_error("Could not create the initial project");
}

ApplicationError noMemory() { return {ApplicationErrorCode::resourceLimit, {}}; }
ApplicationError constructionError(const std::exception& error)
{
    return {ApplicationErrorCode::playbackConstructionFailed, error.what()};
}
project::SessionError recordingBusy()
{
    return {project::SessionErrorCode::recordingActive, "Finish or cancel recording first"};
}

template <class T, class... Errors>
ApplicationResult<T> widen(std::variant<T, Errors...> result)
{
    return std::visit([](auto&& value) -> ApplicationResult<T> {
        return std::forward<decltype(value)>(value);
    }, std::move(result));
}
}

struct ApplicationProject::Playback
{
    std::unique_ptr<tracktion::Edit> edit;
    tracktion::AudioTrack* track = nullptr;
    InstrumentAdapter* instrument = nullptr; // Owned by the edit's plugin list/cache.
    PlaybackConversion conversion;

    ~Playback()
    {
        if (edit != nullptr)
        {
            edit->getTransport().stop(true, true, false);
            edit->getTransport().freePlaybackContext();
        }
    }
};

ApplicationProject::ApplicationProject(tracktion::Engine& owner, double sampleRate)
    : engine(owner), current(initialSession()), rate(sampleRate)
{
    engine.getPluginManager().createBuiltInType<InstrumentAdapter>();
    playback = buildPlayback(current.document(), rate);
}

ApplicationProject::~ApplicationProject() = default;

std::unique_ptr<ApplicationProject::Playback> ApplicationProject::buildPlayback(
    const project::ProjectDocument& document, double sampleRate)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        throw std::invalid_argument("Playback requires a finite positive sample rate");
    auto result = std::make_unique<Playback>();
    result->edit = tracktion::Edit::createSingleTrackEdit(engine);
    if (result->edit == nullptr)
        throw std::runtime_error("Could not create playback");
    result->edit->playInStopEnabled = false;
    result->edit->clickTrackEnabled = false;
    result->edit->tempoSequence.getTempo(0)->setBpm(project::projectTempoBpm);
    const auto tracks = tracktion::getAudioTracks(*result->edit);
    if (tracks.size() != 1)
        throw std::runtime_error("Playback requires one instrument track");
    result->track = tracks[0];
    auto plugin = result->edit->getPluginCache().createNewPlugin(InstrumentAdapter::xmlTypeName, {});
    result->instrument = dynamic_cast<InstrumentAdapter*>(plugin.get());
    if (result->instrument == nullptr)
        throw std::runtime_error("Could not create the shared instrument");
    result->track->pluginList.insertPlugin(plugin, 0, nullptr);
    if (const auto error = result->instrument->applyPatch(document.patch))
        throw std::invalid_argument(error->message);
    result->conversion = derivePlayback(document, *result->track, sampleRate);
    return result;
}

const PlaybackConversion& ApplicationProject::playbackConversion() const noexcept { return playback->conversion; }
tracktion::Edit& ApplicationProject::playbackEdit() const noexcept { return *playback->edit; }
tracktion::AudioTrack& ApplicationProject::playbackTrack() const noexcept { return *playback->track; }
contracts::InstrumentPatch ApplicationProject::playbackPatch() const
{
    requireMessageThread();
    return playback->instrument->currentPatch();
}

std::optional<ApplicationError> ApplicationProject::checkStopped() const
{
    const auto& transport = playback->edit->getTransport();
    if (transport.isPlaying() || transport.isStopping() || transport.isRecordingStopping()
        || playback->edit->isRendering())
        return ApplicationError{ApplicationErrorCode::transportActive, "Stop playback before editing the project"};
    return std::nullopt;
}

ApplicationResult<contracts::EditOutcome> ApplicationProject::changePatch(
    const contracts::InstrumentPatch& patch, const contracts::ProjectCommand* command, bool undo)
{
    try
    {
        requireMessageThread();
        if (const auto error = checkStopped()) return *error;
        if (command != nullptr)
        {
            if (const auto error = contracts::validateCommand(*command)) return *error;
            const auto& snapshot = current.patchSnapshot();
            if (command->projectInstanceId != snapshot.projectInstanceId)
                return contracts::ContractError{contracts::ErrorCode::wrongProject,
                    "project_instance_id", "Command targets a different project instance"};
            if (command->expectedRevision != snapshot.revision)
                return contracts::ContractError{contracts::ErrorCode::staleRevision,
                    "expected_revision", "Command targets an obsolete project revision"};
        }
        if (current.isRecording()) return recordingBusy();
        if (patch == current.document().patch)
            return widen(command != nullptr ? current.apply(*command)
                         : undo ? current.undoPatch() : current.redoPatch());
        auto document = current.document();
        document.patch = patch;
        auto prepared = buildPlayback(document, rate);
        // All throwing graph construction precedes the session mutation. Publication
        // is a pointer swap; failure in session validation/history leaves both intact.
        auto result = command != nullptr ? current.apply(*command)
                    : undo ? current.undoPatch() : current.redoPatch();
        if (std::holds_alternative<contracts::EditOutcome>(result))
            playback.swap(prepared);
        return widen(std::move(result));
    }
    catch (const std::bad_alloc&) { return noMemory(); }
    catch (const std::exception& error) { return constructionError(error); }
}

ApplicationResult<contracts::EditOutcome> ApplicationProject::apply(const contracts::ProjectCommand& command)
{
    return changePatch(command.patch, &command, false);
}

ApplicationResult<contracts::EditOutcome> ApplicationProject::undoPatch()
{
    const auto history = current.undoHistory();
    return changePatch(history.empty() ? current.document().patch : history.back(), nullptr, true);
}

ApplicationResult<contracts::EditOutcome> ApplicationProject::redoPatch()
{
    const auto history = current.redoHistory();
    return changePatch(history.empty() ? current.document().patch : history.back(), nullptr, false);
}

ApplicationStatus ApplicationProject::replace(const project::ProjectDocument& document)
{
    try
    {
        requireMessageThread();
        if (const auto error = checkStopped()) return *error;
        if (current.isRecording()) return recordingBusy();
        if (current.isDirty())
            return project::SessionError{project::SessionErrorCode::unsavedChanges, "Save the current project before replacing it"};
        if (const auto error = project::validateProject(document)) return *error;
        auto prepared = buildPlayback(document, rate);
        auto result = current.reopen(document, freshToken());
        if (std::holds_alternative<std::monostate>(result))
            playback.swap(prepared);
        return widen(std::move(result));
    }
    catch (const std::bad_alloc&) { return noMemory(); }
    catch (const std::exception& error) { return constructionError(error); }
}

ApplicationStatus ApplicationProject::newProject() { return replace({}); }

ApplicationStatus ApplicationProject::open(const juce::File& file)
{
    try
    {
        requireMessageThread();
        auto loaded = project::loadProjectFile(file);
        if (const auto* error = std::get_if<project::ProjectFileError>(&loaded)) return *error;
        return replace(std::get<project::ProjectDocument>(loaded));
    }
    catch (const std::bad_alloc&) { return noMemory(); }
    catch (const std::exception& error) { return constructionError(error); }
}

ApplicationResult<project::SaveReceipt> ApplicationProject::save(const juce::File& file)
{
    try
    {
        requireMessageThread();
        if (current.isRecording()) return recordingBusy();
        // No async save may commit an older version over a newer acknowledged one.
        const auto context = current.context();
        auto result = project::saveProjectFile(file, current.document());
        if (const auto* error = std::get_if<project::ProjectFileError>(&result)) return *error;
        auto status = current.completeSave(context, true);
        return std::visit([&](auto&& value) -> ApplicationResult<project::SaveReceipt> {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::monostate>)
                return std::move(std::get<project::SaveReceipt>(result));
            else
                return std::forward<decltype(value)>(value);
        }, std::move(status));
    }
    catch (const std::bad_alloc&) { return noMemory(); }
    catch (const std::exception& error) { return constructionError(error); }
}

ApplicationStatus ApplicationProject::setSampleRate(double sampleRate)
{
    try
    {
        requireMessageThread();
        if (const auto error = checkStopped()) return *error;
        if (current.isRecording()) return recordingBusy();
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
            return ApplicationError{ApplicationErrorCode::invalidSampleRate, "Playback requires a finite positive sample rate"};
        if (sampleRate == rate) return std::monostate{};
        auto prepared = buildPlayback(current.document(), sampleRate);
        playback.swap(prepared);
        rate = sampleRate;
        return std::monostate{};
    }
    catch (const std::bad_alloc&) { return noMemory(); }
    catch (const std::exception& error) { return constructionError(error); }
}

ApplicationResult<std::unique_ptr<project::RecordingTicket>> ApplicationProject::beginRecording()
{
    requireMessageThread();
    if (const auto error = checkStopped()) return *error;
    return widen(current.beginRecording());
}

ApplicationResult<contracts::EditOutcome> ApplicationProject::commitRecording(
    const project::RecordingTicket& ticket, const project::CapturedMidi& captured, double durationSeconds)
{
    try
    {
        requireMessageThread();
        if (const auto error = checkStopped()) return *error;
        if (!captured.isComplete())
            return ApplicationError{ApplicationErrorCode::incompleteCapture, "The incomplete take was retained; it cannot replace the project"};
        project::ProjectDocument completed{current.document().patch, durationSeconds, captured.events};
        if (const auto error = project::validateProject(completed)) return *error;
        auto prepared = buildPlayback(completed, rate);
        auto result = current.commitRecording(ticket, completed);
        if (std::holds_alternative<contracts::EditOutcome>(result))
            playback.swap(prepared);
        return widen(std::move(result));
    }
    catch (const std::bad_alloc&) { return noMemory(); }
    catch (const std::exception& error) { return constructionError(error); }
}

ApplicationStatus ApplicationProject::cancelRecording(const project::RecordingTicket& ticket)
{
    requireMessageThread();
    if (const auto error = checkStopped()) return *error;
    return widen(current.cancelRecording(ticket));
}

ApplicationStatus ApplicationProject::playFromStart()
{
    requireMessageThread();
    if (current.isRecording()) return recordingBusy();
    if (const auto error = checkStopped()) return *error;
    playback->edit->getTransport().playFromStart(false);
    return std::monostate{};
}

void ApplicationProject::stop()
{
    requireMessageThread();
    playback->edit->getTransport().stop(true, true, false);
    playback->edit->getTransport().freePlaybackContext();
    playback->instrument->reset();
}

void ApplicationProject::panic()
{
    requireMessageThread();
    playback->instrument->midiPanic();
}
}
