#include <composer/contracts/ProjectCommand.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <utility>
#include <vector>

using namespace composer::contracts;

namespace
{
ProjectPatchSession create(std::string id, InstrumentPatch patch = {}, std::uint64_t revision = 0)
{
    auto result = ProjectPatchSession::create(std::move(id), patch, revision);
    REQUIRE(std::holds_alternative<ProjectPatchSession>(result));
    return std::move(std::get<ProjectPatchSession>(result));
}

void applied(const Result<EditOutcome>& result)
{
    REQUIRE(std::holds_alternative<EditOutcome>(result));
    REQUIRE(std::get<EditOutcome>(result) == EditOutcome::applied);
}

void error(const Result<EditOutcome>& result, ErrorCode expected)
{
    REQUIRE(std::holds_alternative<ContractError>(result));
    REQUIRE(std::get<ContractError>(result).code == expected);
}

std::vector<InstrumentPatch> history(std::span<const InstrumentPatch> patches)
{
    return { patches.begin(), patches.end() };
}
}

TEST_CASE("Context change advances the shared revision with empty patch history", "[contracts][context]")
{
    auto session = create("session-a", {}, 41);
    const auto before = session.snapshot();
    const ProjectCommand delayedSamePatch {before.projectInstanceId, before.revision, before.patch};
    applied(session.advanceContext());
    CHECK(session.snapshot().revision == 42);
    CHECK(session.snapshot().projectInstanceId == before.projectInstanceId);
    CHECK(session.snapshot().patch == before.patch);
    CHECK(session.undoHistory().empty());
    CHECK(session.redoHistory().empty());
    error(session.apply(delayedSamePatch), ErrorCode::staleRevision);
    CHECK(session.snapshot().revision == 42);
    auto changed = before.patch;
    changed.gainDb = -18;
    applied(session.apply({"session-a", 42, changed}));
    CHECK(session.snapshot().revision == 43);
    CHECK(session.snapshot().patch == changed);
    CHECK(history(session.undoHistory()) == std::vector<InstrumentPatch>{before.patch});
}

TEST_CASE("Context change preserves populated patch undo and redo and their revisions", "[contracts][context]")
{
    const auto a = InstrumentPatch{};
    auto b = a;
    b.gainDb = -18;
    auto c = a;
    c.gainDb = -24;
    auto session = create("session-a");
    applied(session.apply({"session-a", 0, b}));
    applied(session.apply({"session-a", 1, c}));
    applied(session.undo());
    REQUIRE(session.snapshot().revision == 3);
    REQUIRE(session.snapshot().patch == b);
    const auto undoBefore = history(session.undoHistory());
    const auto redoBefore = history(session.redoHistory());
    REQUIRE(undoBefore == std::vector<InstrumentPatch>{a});
    REQUIRE(redoBefore == std::vector<InstrumentPatch>{c});
    applied(session.advanceContext());
    CHECK(session.snapshot().revision == 4);
    CHECK(session.snapshot().patch == b);
    CHECK(history(session.undoHistory()) == undoBefore);
    CHECK(history(session.redoHistory()) == redoBefore);
    error(session.apply({"session-a", 3, b}), ErrorCode::staleRevision);
    CHECK(history(session.undoHistory()) == undoBefore);
    CHECK(history(session.redoHistory()) == redoBefore);
    applied(session.redo());
    CHECK(session.snapshot().revision == 5);
    CHECK(session.snapshot().patch == c);
    applied(session.undo());
    CHECK(session.snapshot().revision == 6);
    CHECK(session.snapshot().patch == b);
}

TEST_CASE("Context change rejects exhausted revision without losing either patch history", "[contracts][context]")
{
    auto b = InstrumentPatch{};
    b.gainDb = -18;
    auto c = b;
    c.gainDb = -24;
    auto session = create("session-a", {}, maximumProjectRevision - 4);
    applied(session.apply({"session-a", maximumProjectRevision - 4, b}));
    applied(session.apply({"session-a", maximumProjectRevision - 3, c}));
    applied(session.undo());
    REQUIRE(session.snapshot().revision == maximumProjectRevision - 1);
    const auto undoBefore = history(session.undoHistory());
    const auto redoBefore = history(session.redoHistory());
    applied(session.advanceContext());
    REQUIRE(session.snapshot().revision == maximumProjectRevision);
    error(session.advanceContext(), ErrorCode::revisionExhausted);
    CHECK(session.snapshot().revision == maximumProjectRevision);
    CHECK(session.snapshot().patch == b);
    CHECK(session.snapshot().projectInstanceId == "session-a");
    CHECK(history(session.undoHistory()) == undoBefore);
    CHECK(history(session.redoHistory()) == redoBefore);
    error(session.apply({"session-a", maximumProjectRevision - 1, b}), ErrorCode::staleRevision);
    const auto same = session.apply({"session-a", maximumProjectRevision, b});
    REQUIRE(std::holds_alternative<EditOutcome>(same));
    CHECK(std::get<EditOutcome>(same) == EditOutcome::noChange);
    error(session.undo(), ErrorCode::revisionExhausted);
    error(session.redo(), ErrorCode::revisionExhausted);
    CHECK(history(session.undoHistory()) == undoBefore);
    CHECK(history(session.redoHistory()) == redoBefore);
}

TEST_CASE("Reopening with a fresh token rejects old commands even when revisions coincide", "[contracts][context]")
{
    auto old = create("session-a");
    const ProjectCommand delayedAtZero {"session-a", 0, old.snapshot().patch};
    applied(old.advanceContext());
    const ProjectCommand delayedAfterRecording {"session-a", 1, old.snapshot().patch};
    const auto encodedPatch = encodePatch(old.snapshot().patch);
    REQUIRE(std::holds_alternative<std::string>(encodedPatch));
    const auto decodedPatch = decodePatch(std::get<std::string>(encodedPatch));
    REQUIRE(std::holds_alternative<InstrumentPatch>(decodedPatch));
    auto reopened = create("session-b", std::get<InstrumentPatch>(decodedPatch));
    REQUIRE(reopened.snapshot().revision == 0);
    CHECK(reopened.snapshot().patch == old.snapshot().patch);
    CHECK(reopened.undoHistory().empty());
    CHECK(reopened.redoHistory().empty());
    error(reopened.apply(delayedAtZero), ErrorCode::wrongProject);
    error(reopened.apply(delayedAfterRecording), ErrorCode::wrongProject);
    CHECK(reopened.snapshot().revision == 0);
}
