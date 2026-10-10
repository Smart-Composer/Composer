#pragma once

#include "DerivedPlayback.h"
#include <composer/project/MidiCaptureBuffer.h>
#include <composer/project/ProjectFileStore.h>
#include <composer/project/ProjectSession.h>

#include <memory>
#include <variant>

namespace composer::app
{
enum class ApplicationErrorCode
{
    transportActive,
    invalidSampleRate,
    incompleteCapture,
    playbackConstructionFailed,
    resourceLimit
};

struct ApplicationError
{
    ApplicationErrorCode code;
    std::string message;
};

template <class T>
using ApplicationResult = std::variant<T, contracts::ContractError, project::ProjectError,
                                       project::SessionError, project::ProjectFileError, ApplicationError>;
using ApplicationStatus = ApplicationResult<std::monostate>;

// One message-thread owner for the persistent document and its disposable playback.
// The engine outlives this object. This class performs no operation on an audio or
// MIDI callback thread. Editing requires a stopped transport; saves are synchronous.
class ApplicationProject final
{
public:
    // Startup construction can throw. Later operations report exceptions that
    // reach this boundary; this does not contain failures inside JUCE noexcept APIs.
    ApplicationProject(tracktion::Engine&, double sampleRate);
    ~ApplicationProject();
    ApplicationProject(const ApplicationProject&) = delete;
    ApplicationProject& operator=(const ApplicationProject&) = delete;

    const project::ProjectSession& session() const noexcept { return current; }
    const PlaybackConversion& playbackConversion() const noexcept;
    double playbackSampleRate() const noexcept { return rate; }
    contracts::InstrumentPatch playbackPatch() const;

    // Borrowed graph handles for transport/input routing on the message thread.
    // Never retain them across a successful edit, open, completed take or rate change.
    // Original performances are read only through session().document().
    tracktion::Edit& playbackEdit() const noexcept;
    tracktion::AudioTrack& playbackTrack() const noexcept;

    ApplicationResult<contracts::EditOutcome> apply(const contracts::ProjectCommand&);
    ApplicationResult<contracts::EditOutcome> undoPatch();
    ApplicationResult<contracts::EditOutcome> redoPatch();
    ApplicationStatus newProject();
    ApplicationStatus open(const juce::File&);
    ApplicationResult<project::SaveReceipt> save(const juce::File&);
    ApplicationStatus setSampleRate(double);

    // Begin after preparing device/capture storage, before admitting input. The
    // caller must retain the returned ticket, close and join callbacks and validate
    // the clock before commit. Completion/cancellation require that exact attempt's
    // ticket, so a retained old result cannot be accepted as a newer recording.
    // A failed commit preserves the pending attempt and caller-owned originals.
    ApplicationResult<std::unique_ptr<project::RecordingTicket>> beginRecording();
    ApplicationResult<contracts::EditOutcome> commitRecording(
        const project::RecordingTicket&, const project::CapturedMidi&, double durationSeconds);
    ApplicationStatus cancelRecording(const project::RecordingTicket&);

    ApplicationStatus playFromStart();
    void stop();
    void panic();

private:
    struct Playback;
    std::unique_ptr<Playback> buildPlayback(const project::ProjectDocument&, double);
    std::optional<ApplicationError> checkStopped() const;
    ApplicationStatus replace(const project::ProjectDocument&);
    ApplicationResult<contracts::EditOutcome> changePatch(const contracts::InstrumentPatch&,
                                                         const contracts::ProjectCommand*, bool undo);

    tracktion::Engine& engine;
    project::ProjectSession current;
    double rate;
    std::unique_ptr<Playback> playback;
};
}
