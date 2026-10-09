#include <composer/project/MidiCaptureBuffer.h>
#include <composer/project/ProjectFileStore.h>
#include <composer/project/ProjectSession.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

using namespace composer::project;
using namespace composer::contracts;

#if JUCE_WINDOWS
namespace
{
class ScratchDirectory
{
public:
    ScratchDirectory()
        : parent(juce::File::getSpecialLocation(juce::File::tempDirectory)),
          directory(parent.getChildFile("composer-session-file-tests-" + juce::Uuid().toString()))
    {
        const auto resolvedParent = std::filesystem::canonical(
            std::filesystem::path(parent.getFullPathName().toWideCharPointer()));
        const auto resolvedChild = std::filesystem::weakly_canonical(
            std::filesystem::path(directory.getFullPathName().toWideCharPointer()));
        REQUIRE(resolvedChild.parent_path() == resolvedParent);
        REQUIRE_FALSE(directory.exists());
        REQUIRE(directory.createDirectory().wasOk());
        REQUIRE_FALSE(directory.isSymbolicLink());
    }

    ~ScratchDirectory()
    {
        if (directory.getParentDirectory() != parent || directory.isSymbolicLink())
            return;
        // Only direct entries in this uniquely created directory are removed.
        for (const auto& entry : directory.findChildFiles(juce::File::findFilesAndDirectories, false))
            if (entry.getParentDirectory() == directory && !entry.isSymbolicLink())
                entry.deleteFile();
        directory.deleteFile();
    }

    juce::File file(const char* name) const
    {
        const auto result = directory.getChildFile(name);
        REQUIRE(result.getParentDirectory() == directory);
        return result;
    }

    const juce::File& root() const { return directory; }

private:
    juce::File parent;
    juce::File directory;
};

class DenyDeleteReadHandle
{
public:
    explicit DenyDeleteReadHandle(const juce::File& file)
        : handle(CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        REQUIRE(handle != INVALID_HANDLE_VALUE);
    }

    ~DenyDeleteReadHandle() { CloseHandle(handle); }
    DenyDeleteReadHandle(const DenyDeleteReadHandle&) = delete;
    DenyDeleteReadHandle& operator=(const DenyDeleteReadHandle&) = delete;

private:
    HANDLE handle;
};

template <class T, class... Types>
T take(std::variant<Types...> result)
{
    REQUIRE(std::holds_alternative<T>(result));
    return std::move(std::get<T>(result));
}

ProjectDocument exampleDocument()
{
    ProjectDocument document;
    document.durationSeconds = 2.0;
    document.events = {{17.0 / 48000.0, {0x91, 60, 93}},
                       {0.25 + 17.0 / 48000.0, {0x81, 60, 47}},
                       {1.25, {0x92, 67, 81}}, {1.75, {0x82, 67, 31}}};
    return document;
}

void applyGain(ProjectSession& session, double gain)
{
    const auto context = session.context();
    auto patch = session.document().patch;
    patch.gainDb = gain;
    REQUIRE(take<EditOutcome>(session.apply({context.projectInstanceId, context.revision, patch}))
            == EditOutcome::applied);
}

void saveAndAcknowledge(ProjectSession& session, const juce::File& target)
{
    // The trusted editing thread pairs this exact document with its real receipt.
    const auto context = session.context();
    const auto receipt = take<SaveReceipt>(saveProjectFile(target, session.document()));
    REQUIRE(receipt.target == target);
    take<std::monostate>(session.completeSave(context, true));
    REQUIRE_FALSE(session.isDirty());
}

void requireLoaded(const juce::File& file, const ProjectDocument& expected)
{
    CHECK(take<ProjectDocument>(loadProjectFile(file)) == expected);
}

void writeFixture(const juce::File& file, const std::string& bytes)
{
    juce::FileOutputStream stream(file, 0);
    REQUIRE(stream.openedOk());
    REQUIRE(stream.setPosition(0));
    REQUIRE(stream.truncate().wasOk());
    REQUIRE(stream.write(bytes.data(), bytes.size()));
    stream.flush();
    REQUIRE(stream.getStatus().wasOk());
}

using FileReopenResult = std::variant<SessionStatus, ProjectFileError>;

FileReopenResult loadAndReopen(ProjectSession& session, const juce::File& file, std::string token)
{
    auto loaded = loadProjectFile(file);
    if (const auto* error = std::get_if<ProjectFileError>(&loaded))
        return *error;
    return session.reopen(std::get<ProjectDocument>(loaded), std::move(token));
}
}

TEST_CASE("Session capture survives checked file save and typed reopen", "[session][files]")
{
    const ScratchDirectory scratch;
    const auto target = scratch.file("performance.composer");
    auto session = take<ProjectSession>(ProjectSession::create("capture-session"));
    applyGain(session, -21.123456789);
    const auto ticket = take<std::unique_ptr<RecordingTicket>>(session.beginRecording());
    const auto beforeTake = session.context();
    MidiCaptureBuffer capture(240);
    REQUIRE(capture.start() == CaptureStartResult::started);
    std::vector<MidiEvent> originalEvents;
    originalEvents.reserve(240);
    for (int index = 0; index < 120; ++index)
    {
        const auto note = static_cast<std::uint8_t>(48 + index % 24);
        const auto channel = static_cast<std::uint8_t>(index % 3);
        const double start = index * 0.5 + (17 + index % 100) / 48000.0;
        const std::array<std::uint8_t, 3> on {static_cast<std::uint8_t>(0x90 | channel), note, 93};
        const std::array<std::uint8_t, 3> off {static_cast<std::uint8_t>(0x80 | channel), note, 47};
        REQUIRE(capture.submit(start, on) == CaptureSubmitResult::accepted);
        REQUIRE(capture.submit(start + 0.25, off) == CaptureSubmitResult::accepted);
        originalEvents.push_back({start, {on.begin(), on.end()}});
        originalEvents.push_back({start + 0.25, {off.begin(), off.end()}});
    }

    auto captured = capture.finish();
    REQUIRE(captured.has_value());
    REQUIRE(captured->isComplete());
    REQUIRE(captured->events == originalEvents);
    const ProjectDocument completed {session.document().patch, 60.0, std::move(captured->events)};
    REQUIRE(take<EditOutcome>(session.commitRecording(*ticket, completed)) == EditOutcome::applied);
    CHECK(session.document() == completed);
    CHECK(session.isDirty());
    CHECK_FALSE(session.isRecording());
    CHECK(session.context().revision == beforeTake.revision + 1);
    CHECK(take<ContractError>(session.apply(
              {beforeTake.projectInstanceId, beforeTake.revision, completed.patch})).code
          == ErrorCode::staleRevision);

    saveAndAcknowledge(session, target);
    requireLoaded(target, completed);
    const auto savedContext = session.context();
    const auto loaded = take<ProjectDocument>(loadProjectFile(target));
    take<std::monostate>(session.reopen(loaded, "reopened-session"));
    CHECK(session.document() == completed);
    CHECK(session.document().events == originalEvents);
    CHECK_FALSE(session.isDirty());
    CHECK(session.context().projectInstanceId == "reopened-session");
    CHECK(session.context().revision == 0);
    CHECK(session.undoHistory().empty());
    CHECK(session.redoHistory().empty());
    CHECK(take<ContractError>(session.completeSave(savedContext, true)).code == ErrorCode::wrongProject);
    CHECK(take<SessionError>(session.beginRecording()).code == SessionErrorCode::existingPerformance);
    CHECK(session.document() == completed);
}

TEST_CASE("Session replacement failure preserves saved and unsaved performances", "[session][files]")
{
    const ScratchDirectory scratch;
    const auto target = scratch.file("locked.composer");
    const auto original = exampleDocument();
    auto session = take<ProjectSession>(ProjectSession::create("replacement-session", original));
    saveAndAcknowledge(session, target);
    const auto originalBytes = target.loadFileAsString();
    applyGain(session, -30.0);
    const auto unsaved = session.document();
    const auto failedContext = session.context();
    juce::File recovery;
    {
        auto prepared = take<PreparedProjectSave>(PreparedProjectSave::prepare(target, unsaved));
        {
            const DenyDeleteReadHandle held(prepared.stagedFile());
            const auto error = take<ProjectFileError>(prepared.commit());
            CHECK(error.code == ProjectFileErrorCode::commitFailed);
            recovery = error.recoveryFile;
            REQUIRE(recovery.existsAsFile());
            CHECK(target.loadFileAsString() == originalBytes);
        }
        CHECK(take<ProjectFileError>(prepared.commit()).code == ProjectFileErrorCode::alreadyCommitted);
    }

    CHECK(take<SessionError>(session.completeSave(failedContext, false)).code == SessionErrorCode::saveFailed);
    CHECK(session.isDirty());
    CHECK(session.document() == unsaved);
    CHECK(session.context().projectInstanceId == failedContext.projectInstanceId);
    CHECK(session.context().revision == failedContext.revision);
    requireLoaded(target, original);
    const auto recovered = take<ProjectDocument>(loadProjectFile(recovery));
    CHECK((recovered == original || recovered == unsaved));

    // Retry creates a new prepared save; the failed attempt is consumed.
    saveAndAcknowledge(session, target);
    requireLoaded(target, unsaved);
    CHECK(session.document().events == original.events);
}

TEST_CASE("A delayed actual file receipt cannot clear newer session edits", "[session][files]")
{
    const ScratchDirectory scratch;
    const auto target = scratch.file("delayed.composer");
    const auto original = exampleDocument();
    auto session = take<ProjectSession>(ProjectSession::create("delayed-session", original));
    saveAndAcknowledge(session, target);
    applyGain(session, -27.0);
    const auto delayedContext = session.context();
    const auto delayedDocument = session.document();
    auto delayed = take<PreparedProjectSave>(PreparedProjectSave::prepare(target, delayedDocument));
    applyGain(session, -18.0);
    const auto latest = session.document();
    const auto latestContext = session.context();

    const auto receipt = take<SaveReceipt>(delayed.commit());
    CHECK(receipt.target == target);
    CHECK(take<ContractError>(session.completeSave(delayedContext, true)).code == ErrorCode::staleRevision);
    CHECK(session.isDirty());
    CHECK(session.document() == latest);
    CHECK(session.context().revision == latestContext.revision);
    CHECK(session.document().events == original.events);
    requireLoaded(target, delayedDocument);
    CHECK(take<SessionError>(session.reopen(delayedDocument, "blocked-reopen")).code
          == SessionErrorCode::unsavedChanges);
    CHECK(session.document() == latest);

    saveAndAcknowledge(session, target);
    requireLoaded(target, latest);
}

TEST_CASE("Rejected file loads leave the current session intact", "[session][files]")
{
    const ScratchDirectory scratch;
    auto session = take<ProjectSession>(ProjectSession::create("retained-session", exampleDocument()));
    applyGain(session, -24.0);
    const auto original = session.document();
    const auto context = session.context();
    const std::vector<InstrumentPatch> undo(session.undoHistory().begin(), session.undoHistory().end());
    const std::vector<InstrumentPatch> redo(session.redoHistory().begin(), session.redoHistory().end());
    auto input = scratch.file("missing.composer");
    auto expected = ProjectFileErrorCode::missingFile;

    SECTION("Missing file") {}
    SECTION("Empty file")
    {
        input = scratch.file("empty.composer");
        writeFixture(input, "");
        expected = ProjectFileErrorCode::emptyFile;
    }
    SECTION("Malformed file")
    {
        input = scratch.file("malformed.composer");
        writeFixture(input, "{not a project");
        expected = ProjectFileErrorCode::invalidDocument;
    }
    SECTION("Directory instead of a regular file")
    {
        input = scratch.root();
        expected = ProjectFileErrorCode::invalidTarget;
    }

    // The loader must succeed before a typed document reaches the session.
    CHECK(take<ProjectFileError>(loadAndReopen(session, input, "unused-replacement")).code == expected);
    CHECK(session.document() == original);
    CHECK(session.isDirty());
    CHECK_FALSE(session.isRecording());
    CHECK(session.context().projectInstanceId == context.projectInstanceId);
    CHECK(session.context().revision == context.revision);
    CHECK(std::vector<InstrumentPatch>(session.undoHistory().begin(), session.undoHistory().end()) == undo);
    CHECK(std::vector<InstrumentPatch>(session.redoHistory().begin(), session.redoHistory().end()) == redo);
}
#else
TEST_CASE("Session file integration reports unsupported storage platforms", "[session][files]")
{
    const auto loaded = loadProjectFile(juce::File());
    REQUIRE(std::holds_alternative<ProjectFileError>(loaded));
    CHECK(std::get<ProjectFileError>(loaded).code == ProjectFileErrorCode::unsupportedPlatform);
}
#endif
