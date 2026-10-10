#include <composer/project/ProjectSession.h>

#include <limits>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace composer::project
{
using namespace contracts;

namespace
{
SessionError sessionError(SessionErrorCode code, const char* message)
{
    return {code, message};
}

SessionError noMemory()
{
    return {SessionErrorCode::resourceLimit, {}};
}

SessionError recordingBusy()
{
    return sessionError(SessionErrorCode::recordingActive, "Finish or cancel recording first");
}
}

struct ProjectSession::State
{
    State(ProjectDocument value, ProjectPatchSession edits)
        : document(std::move(value)), patches(std::move(edits)) {}

    ProjectDocument document;
    ProjectPatchSession patches;
    bool dirty = false;
    std::optional<std::uint64_t> activeAttempt;
    std::uint64_t lastAttempt = 0;
};

ProjectSession::ProjectSession(std::unique_ptr<State> initial) : state(std::move(initial)) {}
ProjectSession::ProjectSession(ProjectSession&&) noexcept = default;
ProjectSession& ProjectSession::operator=(ProjectSession&&) noexcept = default;
ProjectSession::~ProjectSession() = default;

SessionResult<ProjectSession> ProjectSession::create(
    std::string freshToken, ProjectDocument document, std::uint64_t initialRevision)
{
    try
    {
        if (auto error = validateProject(document))
            return *error;
        auto edits = ProjectPatchSession::create(std::move(freshToken), document.patch, initialRevision);
        if (const auto* error = std::get_if<ContractError>(&edits))
            return *error;
        return ProjectSession(std::make_unique<State>(std::move(document),
                              std::move(std::get<ProjectPatchSession>(edits))));
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}

const ProjectDocument& ProjectSession::document() const noexcept { return state->document; }
const ProjectPatchSnapshot& ProjectSession::patchSnapshot() const noexcept { return state->patches.snapshot(); }
std::span<const InstrumentPatch> ProjectSession::undoHistory() const noexcept { return state->patches.undoHistory(); }
std::span<const InstrumentPatch> ProjectSession::redoHistory() const noexcept { return state->patches.redoHistory(); }
ProjectContext ProjectSession::context() const { return {patchSnapshot().projectInstanceId, patchSnapshot().revision}; }
bool ProjectSession::isDirty() const noexcept { return state->dirty; }
bool ProjectSession::isRecording() const noexcept { return state->activeAttempt.has_value(); }

std::optional<ContractError> ProjectSession::checkContext(const ProjectContext& expected) const
{
    if (auto error = validateCommand({expected.projectInstanceId, expected.revision, document().patch}))
        return error;
    if (expected.projectInstanceId != patchSnapshot().projectInstanceId)
        return ContractError{ErrorCode::wrongProject, "project_instance_id", "Result belongs to another project"};
    if (expected.revision != patchSnapshot().revision)
        return ContractError{ErrorCode::staleRevision, "expected_revision", "Result belongs to an obsolete revision"};
    return std::nullopt;
}

std::optional<SessionError> ProjectSession::checkRecording(const RecordingTicket& ticket) const
{
    if (!isRecording())
        return sessionError(SessionErrorCode::noRecording, "No recording is awaiting completion");
    if (ticket.attempt != *state->activeAttempt)
        return sessionError(SessionErrorCode::obsoleteRecording, "Recording attempt is no longer active");
    return std::nullopt;
}

SessionResult<EditOutcome> ProjectSession::finishPatchEdit(Result<EditOutcome> result)
{
    if (const auto* error = std::get_if<ContractError>(&result))
        return *error;
    const auto outcome = std::get<EditOutcome>(result);
    if (outcome == EditOutcome::applied)
    {
        static_assert(std::is_nothrow_copy_assignable_v<InstrumentPatch>);
        state->document.patch = patchSnapshot().patch;
        state->dirty = true;
    }
    return outcome;
}

SessionResult<EditOutcome> ProjectSession::apply(const ProjectCommand& command)
{
    try
    {
        if (auto error = validateCommand(command))
            return *error;
        if (auto error = checkContext({command.projectInstanceId, command.expectedRevision}))
            return *error;
        if (isRecording())
            return recordingBusy();
        return finishPatchEdit(state->patches.apply(command));
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}

SessionResult<EditOutcome> ProjectSession::undoPatch()
{
    try
    {
        if (isRecording()) return recordingBusy();
        return finishPatchEdit(state->patches.undo());
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}

SessionResult<EditOutcome> ProjectSession::redoPatch()
{
    try
    {
        if (isRecording()) return recordingBusy();
        return finishPatchEdit(state->patches.redo());
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}

SessionResult<std::unique_ptr<RecordingTicket>> ProjectSession::beginRecording()
{
    try
    {
        if (isRecording()) return recordingBusy();
        if (!document().events.empty() || document().durationSeconds != 0.0)
            return sessionError(SessionErrorCode::existingPerformance, "Save this performance and create a new project first");
        if (state->lastAttempt == std::numeric_limits<std::uint64_t>::max())
            return sessionError(SessionErrorCode::recordingAttemptsExhausted, "Recording attempt identifiers are exhausted");
        auto ticket = std::make_unique<RecordingTicket>();
        ticket->context = context();
        ticket->attempt = state->lastAttempt + 1;
        static_assert(std::is_nothrow_constructible_v<SessionResult<std::unique_ptr<RecordingTicket>>,
                                                     std::unique_ptr<RecordingTicket>&&>);
        state->lastAttempt = ticket->attempt;
        state->activeAttempt = ticket->attempt;
        return ticket;
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}

SessionStatus ProjectSession::cancelRecording(const RecordingTicket& ticket)
{
    try
    {
        if (auto error = checkContext(ticket.context)) return *error;
        if (auto error = checkRecording(ticket)) return *error;
        state->activeAttempt.reset();
        return std::monostate{};
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}

SessionResult<EditOutcome> ProjectSession::commitRecording(
    const RecordingTicket& ticket, const ProjectDocument& completed)
{
    try
    {
        if (auto error = validateProject(completed)) return *error;
        if (auto error = checkContext(ticket.context)) return *error;
        if (auto error = checkRecording(ticket)) return *error;
        if (completed.patch != document().patch)
            return sessionError(SessionErrorCode::recordingPatchChanged, "A recording cannot replace its frozen instrument patch");
        if (completed == document())
        {
            state->activeAttempt.reset();
            return EditOutcome::noChange;
        }

        ProjectDocument prepared = completed;
        const auto advanced = state->patches.advanceContext();
        if (const auto* error = std::get_if<ContractError>(&advanced)) return *error;
        using std::swap;
        static_assert(noexcept(swap(state->document.patch, prepared.patch)));
        static_assert(noexcept(swap(state->document.durationSeconds, prepared.durationSeconds)));
        static_assert(noexcept(state->document.events.swap(prepared.events)));
        swap(state->document.patch, prepared.patch);
        swap(state->document.durationSeconds, prepared.durationSeconds);
        state->document.events.swap(prepared.events);
        state->dirty = true;
        state->activeAttempt.reset();
        return EditOutcome::applied;
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}

SessionStatus ProjectSession::install(ProjectDocument prepared, std::string freshToken)
{
    if (isRecording()) return recordingBusy();
    if (isDirty())
        return sessionError(SessionErrorCode::unsavedChanges, "Save the current project before replacing it");
    if (freshToken == patchSnapshot().projectInstanceId)
        return sessionError(SessionErrorCode::reusedToken, "Opening a project requires a fresh instance token");
    auto edits = ProjectPatchSession::create(std::move(freshToken), prepared.patch);
    if (const auto* error = std::get_if<ContractError>(&edits)) return *error;
    auto replacement = std::make_unique<State>(std::move(prepared), std::move(std::get<ProjectPatchSession>(edits)));
    state.swap(replacement);
    return std::monostate{};
}

SessionStatus ProjectSession::reopen(std::string_view encoded, std::string freshToken)
{
    try
    {
        auto decoded = decodeProject(encoded);
        if (const auto* error = std::get_if<ProjectError>(&decoded)) return *error;
        return install(std::move(std::get<ProjectDocument>(decoded)), std::move(freshToken));
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}

SessionStatus ProjectSession::reopen(const ProjectDocument& document, std::string freshToken)
{
    try
    {
        if (auto error = validateProject(document)) return *error;
        ProjectDocument prepared = document;
        return install(std::move(prepared), std::move(freshToken));
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}

SessionStatus ProjectSession::newProject(std::string freshToken)
{
    try { return install({}, std::move(freshToken)); }
    catch (const std::bad_alloc&) { return noMemory(); }
}

SessionStatus ProjectSession::completeSave(const ProjectContext& savedContext, bool receivedSaveReceipt)
{
    try
    {
        if (!receivedSaveReceipt)
            return sessionError(SessionErrorCode::saveFailed, "Save failed; unsaved state is unchanged");
        if (auto error = checkContext(savedContext)) return *error;
        state->dirty = false;
        return std::monostate{};
    }
    catch (const std::bad_alloc&) { return noMemory(); }
}
}
