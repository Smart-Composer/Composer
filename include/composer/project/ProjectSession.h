#pragma once

#include <composer/contracts/ProjectCommand.h>
#include <composer/project/ProjectDocument.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace composer::project
{
struct ProjectContext
{
    std::string projectInstanceId;
    std::uint64_t revision = 0;
};

struct RecordingTicket
{
    ProjectContext context;
    std::uint64_t attempt = 0;
};

enum class SessionErrorCode
{
    recordingActive,
    noRecording,
    obsoleteRecording,
    existingPerformance,
    unsavedChanges,
    reusedToken,
    recordingPatchChanged,
    saveFailed,
    resourceLimit,
    recordingAttemptsExhausted
};

struct SessionError
{
    SessionErrorCode code;
    std::string message;
};

template <class T>
using SessionResult = std::variant<T, contracts::ContractError, ProjectError, SessionError>;
using SessionStatus = SessionResult<std::monostate>;

// One editing thread owns the session. No method is an audio/MIDI callback API.
// Moved-from sessions may only be destroyed or assigned. Const views are for use
// before the next session mutation; retain a copy for asynchronous work.
class ProjectSession
{
public:
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;
    ProjectSession(ProjectSession&&) noexcept;
    ProjectSession& operator=(ProjectSession&&) noexcept;
    ~ProjectSession();

    // Accepts a clean initial document. The owner must generate a globally fresh
    // token; normal new/open flows use revision zero.
    static SessionResult<ProjectSession> create(
        std::string freshToken, ProjectDocument document = {}, std::uint64_t initialRevision = 0);

    const ProjectDocument& document() const noexcept;
    const contracts::ProjectPatchSnapshot& patchSnapshot() const noexcept;
    std::span<const contracts::InstrumentPatch> undoHistory() const noexcept;
    std::span<const contracts::InstrumentPatch> redoHistory() const noexcept;
    ProjectContext context() const;
    bool isDirty() const noexcept;
    bool isRecording() const noexcept;

    SessionResult<contracts::EditOutcome> apply(const contracts::ProjectCommand& command);
    SessionResult<contracts::EditOutcome> undoPatch();
    SessionResult<contracts::EditOutcome> redoPatch();

    // The owner retains the ticket until completion or explicit cancellation.
    // Before commit, quiesce capture and reject any incomplete capture result.
    // Failed completion preserves the caller's document and pending ticket.
    SessionResult<std::unique_ptr<RecordingTicket>> beginRecording();
    SessionStatus cancelRecording(const RecordingTicket& ticket);
    SessionResult<contracts::EditOutcome> commitRecording(
        const RecordingTicket& ticket, const ProjectDocument& completed);

    // Open/new require a clean idle session and a globally fresh token. Only
    // reuse of the current token is checked here. Patch histories are reset.
    SessionStatus reopen(std::string_view encoded, std::string freshToken);
    SessionStatus reopen(const ProjectDocument& document, std::string freshToken);
    SessionStatus newProject(std::string freshToken);

    // Trusted application bookkeeping only: true means a real save receipt for
    // this context's exact document. Capture that context with the saved document,
    // not when a delayed receipt arrives. Never expose this as a language command.
    SessionStatus completeSave(const ProjectContext& savedContext, bool receivedSaveReceipt);

private:
    struct State;
    explicit ProjectSession(std::unique_ptr<State> initial);
    SessionStatus install(ProjectDocument prepared, std::string freshToken);
    SessionResult<contracts::EditOutcome> finishPatchEdit(contracts::Result<contracts::EditOutcome> result);
    std::optional<contracts::ContractError> checkContext(const ProjectContext& expected) const;
    std::optional<SessionError> checkRecording(const RecordingTicket& ticket) const;
    std::unique_ptr<State> state;
};
}
