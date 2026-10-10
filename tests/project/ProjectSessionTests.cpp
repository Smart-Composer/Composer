#include <composer/project/ProjectSession.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace composer::project;
using namespace composer::contracts;

namespace
{
template <class T>
T take(SessionResult<T> result)
{
    REQUIRE(std::holds_alternative<T>(result));
    return std::move(std::get<T>(result));
}

template <class T>
void sessionError(const SessionResult<T>& result, SessionErrorCode code)
{
    REQUIRE(std::holds_alternative<SessionError>(result));
    CHECK(std::get<SessionError>(result).code == code);
}

template <class T>
void contractError(const SessionResult<T>& result, ErrorCode code)
{
    REQUIRE(std::holds_alternative<ContractError>(result));
    CHECK(std::get<ContractError>(result).code == code);
}

void applied(const SessionResult<EditOutcome>& result)
{
    REQUIRE(std::holds_alternative<EditOutcome>(result));
    CHECK(std::get<EditOutcome>(result) == EditOutcome::applied);
}

std::string encoded(const ProjectDocument& document)
{
    auto result = encodeProject(document);
    REQUIRE(std::holds_alternative<std::string>(result));
    return std::get<std::string>(std::move(result));
}

ProjectDocument performance(InstrumentPatch patch = {})
{
    return {patch, 60.0, {{0.125, {0x90, 60, 100}}, {0.125, {0xb0, 1, 67}},
                          {59.75, {0x80, 60, 45}}}};
}

struct Snapshot
{
    ProjectDocument document;
    ProjectContext context;
    std::vector<InstrumentPatch> undo;
    std::vector<InstrumentPatch> redo;
    bool dirty;
    bool recording;
};

Snapshot snapshot(const ProjectSession& session)
{
    return {session.document(), session.context(),
            {session.undoHistory().begin(), session.undoHistory().end()},
            {session.redoHistory().begin(), session.redoHistory().end()},
            session.isDirty(), session.isRecording()};
}

void unchanged(const ProjectSession& session, const Snapshot& before)
{
    CHECK(session.document() == before.document);
    CHECK(session.context().projectInstanceId == before.context.projectInstanceId);
    CHECK(session.context().revision == before.context.revision);
    CHECK(session.patchSnapshot().patch == session.document().patch);
    const std::vector<InstrumentPatch> undo(session.undoHistory().begin(), session.undoHistory().end());
    const std::vector<InstrumentPatch> redo(session.redoHistory().begin(), session.redoHistory().end());
    CHECK(undo == before.undo);
    CHECK(redo == before.redo);
    CHECK(session.isDirty() == before.dirty);
    CHECK(session.isRecording() == before.recording);
}
}

TEST_CASE("Session patch edits and patch history preserve original MIDI", "[session]")
{
    const auto original = performance();
    auto session = take(ProjectSession::create("session-a", original));
    auto changed = original.patch;
    changed.gainDb = -18;
    applied(session.apply({"session-a", 0, changed}));
    CHECK(session.document().events == original.events);
    CHECK(session.document().durationSeconds == original.durationSeconds);
    CHECK(session.document().patch == changed);
    CHECK(session.isDirty());
    applied(session.undoPatch());
    CHECK(session.document() == original);
    CHECK(session.patchSnapshot().revision == 2);
    CHECK(session.isDirty());
    applied(session.redoPatch());
    CHECK(session.document().patch == changed);
    CHECK(session.document().events == original.events);
    const auto before = snapshot(session);
    CHECK(take(session.apply({"session-a", 3, changed})) == EditOutcome::noChange);
    unchanged(session, before);
    auto invalid = changed;
    invalid.cutoffHz = 0;
    contractError(session.apply({"session-b", 2, invalid}), ErrorCode::invalidCommand);
    unchanged(session, before);
    contractError(session.apply({"session-b", 2, changed}), ErrorCode::wrongProject);
    unchanged(session, before);
    contractError(session.apply({"session-a", 2, changed}), ErrorCode::staleRevision);
    unchanged(session, before);
}

TEST_CASE("Recording completion validates before a shared revision and document commit", "[session]")
{
    auto session = take(ProjectSession::create("session-a"));
    auto b = InstrumentPatch{};
    b.gainDb = -18;
    auto c = b;
    c.gainDb = -24;
    applied(session.apply({"session-a", 0, b}));
    applied(session.apply({"session-a", 1, c}));
    applied(session.undoPatch());
    const auto ticketOwner = take(session.beginRecording());
    const auto& ticket = *ticketOwner;
    const auto before = snapshot(session);
    sessionError(session.apply({"session-a", 3, c}), SessionErrorCode::recordingActive);
    sessionError(session.undoPatch(), SessionErrorCode::recordingActive);
    sessionError(session.redoPatch(), SessionErrorCode::recordingActive);
    sessionError(session.reopen(encoded(performance()), "session-b"), SessionErrorCode::recordingActive);
    sessionError(session.newProject("session-b"), SessionErrorCode::recordingActive);
    unchanged(session, before);

    auto completed = performance(b);
    auto invalid = completed;
    invalid.events.back().timeSeconds = -1;
    REQUIRE(std::holds_alternative<ProjectError>(session.commitRecording(ticket, invalid)));
    unchanged(session, before);
    auto smuggled = completed;
    smuggled.patch = c;
    sessionError(session.commitRecording(ticket, smuggled), SessionErrorCode::recordingPatchChanged);
    unchanged(session, before);
    auto wrong = ticket;
    wrong.context.projectInstanceId = "session-b";
    contractError(session.commitRecording(wrong, completed), ErrorCode::wrongProject);
    unchanged(session, before);
    wrong = ticket;
    --wrong.context.revision;
    contractError(session.commitRecording(wrong, completed), ErrorCode::staleRevision);
    unchanged(session, before);
    wrong = ticket;
    ++wrong.attempt;
    sessionError(session.commitRecording(wrong, completed), SessionErrorCode::obsoleteRecording);
    unchanged(session, before);

    applied(session.commitRecording(ticket, completed));
    CHECK(session.document() == completed);
    CHECK(session.patchSnapshot().revision == 4);
    CHECK_FALSE(session.isRecording());
    CHECK(session.isDirty());
    const std::vector<InstrumentPatch> undo(session.undoHistory().begin(), session.undoHistory().end());
    const std::vector<InstrumentPatch> redo(session.redoHistory().begin(), session.redoHistory().end());
    CHECK(undo == before.undo);
    CHECK(redo == before.redo);
    const auto after = snapshot(session);
    contractError(session.apply({"session-a", 3, b}), ErrorCode::staleRevision);
    unchanged(session, after);
    applied(session.redoPatch());
    CHECK(session.document().patch == c);
    CHECK(session.document().events == completed.events);
    applied(session.undoPatch());
    CHECK(session.document() == completed);
    CHECK(session.patchSnapshot().revision == 6);
}

TEST_CASE("Recording attempt identity rejects cancelled takes and empty completion is a no-op", "[session]")
{
    auto session = take(ProjectSession::create("session-a"));
    const auto firstOwner = take(session.beginRecording());
    const auto& first = *firstOwner;
    take(session.cancelRecording(first));
    CHECK(session.patchSnapshot().revision == 0);
    CHECK_FALSE(session.isDirty());
    const auto secondOwner = take(session.beginRecording());
    const auto& second = *secondOwner;
    CHECK(second.context.revision == first.context.revision);
    CHECK(second.attempt != first.attempt);
    const auto before = snapshot(session);
    sessionError(session.commitRecording(first, performance()), SessionErrorCode::obsoleteRecording);
    sessionError(session.cancelRecording(first), SessionErrorCode::obsoleteRecording);
    unchanged(session, before);
    CHECK(take(session.commitRecording(second, session.document())) == EditOutcome::noChange);
    CHECK_FALSE(session.isRecording());
    CHECK_FALSE(session.isDirty());
    CHECK(session.patchSnapshot().revision == 0);
    const auto thirdOwner = take(session.beginRecording());
    const auto& third = *thirdOwner;
    CHECK(third.attempt != second.attempt);
    ProjectDocument silence;
    silence.durationSeconds = 1.0;
    applied(session.commitRecording(third, silence));
    CHECK(session.patchSnapshot().revision == 1);
    CHECK(session.isDirty());
    sessionError(session.beginRecording(), SessionErrorCode::existingPerformance);
}

TEST_CASE("Save acknowledgments and reopen preserve unsaved state until the exact current save succeeds", "[session]")
{
    auto session = take(ProjectSession::create("session-a", performance()));
    const auto oldSave = session.context();
    auto b = session.document().patch;
    b.gainDb = -18;
    applied(session.apply({"session-a", 0, b}));
    const auto before = snapshot(session);
    REQUIRE(std::holds_alternative<ProjectError>(session.reopen("{broken", "session-b")));
    unchanged(session, before);
    sessionError(session.reopen(encoded(performance()), "session-b"), SessionErrorCode::unsavedChanges);
    unchanged(session, before);
    sessionError(session.completeSave(session.context(), false), SessionErrorCode::saveFailed);
    unchanged(session, before);
    contractError(session.completeSave(oldSave, true), ErrorCode::staleRevision);
    unchanged(session, before);
    contractError(session.completeSave({"another-session", 1}, true), ErrorCode::wrongProject);
    unchanged(session, before);
    take(session.completeSave(session.context(), true));
    CHECK_FALSE(session.isDirty());

    const auto clean = snapshot(session);
    sessionError(session.reopen(encoded(performance()), "session-a"), SessionErrorCode::reusedToken);
    unchanged(session, clean);
    contractError(session.reopen(encoded(performance()), "bad token"), ErrorCode::invalidCommand);
    unchanged(session, clean);
    const ProjectCommand oldCommandAtZero{"session-a", 0, {}};
    const ProjectCommand oldCommandAtOne{"session-a", 1, b};
    const auto reopenedDocument = session.document();
    take(session.reopen(encoded(reopenedDocument), "session-b"));
    CHECK(session.document() == reopenedDocument);
    CHECK(session.context().projectInstanceId == "session-b");
    CHECK(session.context().revision == 0);
    CHECK(session.undoHistory().empty());
    CHECK(session.redoHistory().empty());
    CHECK_FALSE(session.isDirty());
    const auto reopened = snapshot(session);
    contractError(session.apply(oldCommandAtZero), ErrorCode::wrongProject);
    contractError(session.apply(oldCommandAtOne), ErrorCode::wrongProject);
    contractError(session.completeSave(oldSave, true), ErrorCode::wrongProject);
    unchanged(session, reopened);
}

TEST_CASE("Original performances require save and new before another recording", "[session]")
{
    auto session = take(ProjectSession::create("session-a"));
    const auto ticketOwner = take(session.beginRecording());
    const auto& ticket = *ticketOwner;
    const auto original = performance();
    applied(session.commitRecording(ticket, original));
    const auto before = snapshot(session);
    sessionError(session.beginRecording(), SessionErrorCode::existingPerformance);
    sessionError(session.newProject("session-b"), SessionErrorCode::unsavedChanges);
    sessionError(session.completeSave(session.context(), false), SessionErrorCode::saveFailed);
    unchanged(session, before);
    take(session.completeSave(session.context(), true));
    sessionError(session.beginRecording(), SessionErrorCode::existingPerformance);
    take(session.newProject("session-b"));
    CHECK(session.document() == ProjectDocument{});
    CHECK(session.context().revision == 0);
    CHECK_FALSE(session.isDirty());
    take(session.beginRecording());
    CHECK(session.isRecording());
}

TEST_CASE("Revision exhaustion rejects recording atomically but permits an identical empty completion", "[session]")
{
    auto last = take(ProjectSession::create("session-a", {}, maximumProjectRevision - 1));
    const auto lastTicketOwner = take(last.beginRecording());
    const auto& lastTicket = *lastTicketOwner;
    applied(last.commitRecording(lastTicket, performance()));
    CHECK(last.context().revision == maximumProjectRevision);

    auto full = take(ProjectSession::create("session-b", {}, maximumProjectRevision));
    const auto ticketOwner = take(full.beginRecording());
    const auto& ticket = *ticketOwner;
    const auto before = snapshot(full);
    const auto completed = performance();
    contractError(full.commitRecording(ticket, completed), ErrorCode::revisionExhausted);
    unchanged(full, before);
    CHECK(completed == performance());
    CHECK(take(full.commitRecording(ticket, full.document())) == EditOutcome::noChange);
    CHECK_FALSE(full.isRecording());
    CHECK_FALSE(full.isDirty());
    CHECK(full.context().revision == maximumProjectRevision);
    auto changed = full.document().patch;
    changed.gainDb = -18;
    const auto idle = snapshot(full);
    contractError(full.apply({"session-b", maximumProjectRevision, changed}), ErrorCode::revisionExhausted);
    unchanged(full, idle);
    CHECK(take(full.apply({"session-b", maximumProjectRevision, full.document().patch})) == EditOutcome::noChange);
    unchanged(full, idle);
}

TEST_CASE("Typed reopen validates and prepares before replacing a clean session", "[session]")
{
    const auto original = performance();
    auto session = take(ProjectSession::create("session-a", original));
    auto replacement = performance();
    replacement.patch.gainDb = -30;
    replacement.events.front().bytes = {0x91, 48, 73};
    const auto preservedReplacement = replacement;

    SECTION("Invalid documents and tokens retain both session and input")
    {
        const auto before = snapshot(session);
        auto invalid = replacement;
        invalid.events.back().timeSeconds = -1;
        const auto preservedInvalid = invalid;
        REQUIRE(std::holds_alternative<ProjectError>(session.reopen(invalid, "session-b")));
        unchanged(session, before);
        CHECK(invalid == preservedInvalid);
        sessionError(session.reopen(replacement, "session-a"), SessionErrorCode::reusedToken);
        unchanged(session, before);
        contractError(session.reopen(replacement, "bad token"), ErrorCode::invalidCommand);
        unchanged(session, before);
        CHECK(replacement == preservedReplacement);
    }

    SECTION("Dirty and recording guards retain both session and input")
    {
        auto patch = original.patch;
        patch.gainDb = -18;
        applied(session.apply({"session-a", 0, patch}));
        const auto before = snapshot(session);
        sessionError(session.reopen(replacement, "session-b"), SessionErrorCode::unsavedChanges);
        unchanged(session, before);
        CHECK(replacement == preservedReplacement);

        auto recording = take(ProjectSession::create("recording-a"));
        const auto ticket = take(recording.beginRecording());
        const auto armed = snapshot(recording);
        sessionError(recording.reopen(replacement, "recording-b"), SessionErrorCode::recordingActive);
        unchanged(recording, armed);
        CHECK(replacement == preservedReplacement);
        take(recording.cancelRecording(*ticket));
    }

    SECTION("Successful typed open clears history and invalidates the prior project")
    {
        auto patch = original.patch;
        patch.gainDb = -18;
        applied(session.apply({"session-a", 0, patch}));
        patch.gainDb = -24;
        applied(session.apply({"session-a", 1, patch}));
        applied(session.undoPatch());
        REQUIRE_FALSE(session.undoHistory().empty());
        REQUIRE_FALSE(session.redoHistory().empty());
        const auto saved = session.context();
        take(session.completeSave(saved, true));
        take(session.reopen(replacement, "session-b"));
        CHECK(session.document() == preservedReplacement);
        CHECK(replacement == preservedReplacement);
        CHECK(session.context().projectInstanceId == "session-b");
        CHECK(session.context().revision == 0);
        CHECK(session.undoHistory().empty());
        CHECK(session.redoHistory().empty());
        CHECK_FALSE(session.isDirty());
        CHECK_FALSE(session.isRecording());
        const auto reopened = snapshot(session);
        contractError(session.completeSave(saved, true), ErrorCode::wrongProject);
        unchanged(session, reopened);
    }
}

TEST_CASE("A receipt during recording applies only to the persistent pre-take document", "[session]")
{
    auto session = take(ProjectSession::create("session-a"));
    auto patch = session.document().patch;
    patch.gainDb = -18;
    applied(session.apply({"session-a", 0, patch}));
    const auto ticket = take(session.beginRecording());
    const auto saved = session.context();
    take(session.completeSave(saved, true));
    CHECK_FALSE(session.isDirty());
    CHECK(session.isRecording());

    SECTION("An effective take invalidates the receipt and becomes dirty")
    {
        applied(session.commitRecording(*ticket, performance(patch)));
        CHECK(session.context().revision == saved.revision + 1);
        CHECK(session.isDirty());
        const auto before = snapshot(session);
        contractError(session.completeSave(saved, true), ErrorCode::staleRevision);
        unchanged(session, before);
    }

    SECTION("Empty completion retains the persistent context and clean state")
    {
        CHECK(take(session.commitRecording(*ticket, session.document())) == EditOutcome::noChange);
        CHECK(session.context().revision == saved.revision);
        CHECK_FALSE(session.isRecording());
        CHECK_FALSE(session.isDirty());
        take(session.completeSave(saved, true));
    }

    SECTION("Cancellation retains the persistent context and clean state")
    {
        take(session.cancelRecording(*ticket));
        CHECK(session.context().revision == saved.revision);
        CHECK_FALSE(session.isRecording());
        CHECK_FALSE(session.isDirty());
        take(session.completeSave(saved, true));
    }
}
