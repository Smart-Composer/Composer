#include <composer/project/ProjectFileStore.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <cwchar>
#include <limits>
#include <string>
#include <utility>

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

#if JUCE_WINDOWS
namespace
{
class ScratchDirectory
{
public:
    explicit ScratchDirectory(int pathLength = 0)
        : parent(juce::File::getSpecialLocation(juce::File::tempDirectory)),
          directory(parent.getChildFile("composer-project-file-tests-" + juce::Uuid().toString()))
    {
        if (pathLength != 0)
        {
            const auto current = static_cast<int>(std::wcslen(directory.getFullPathName().toWideCharPointer()));
            REQUIRE(pathLength >= current);
            directory = directory.getSiblingFile(directory.getFileName()
                          + juce::String::repeatedString("x", pathLength - current));
            REQUIRE(std::wcslen(directory.getFullPathName().toWideCharPointer()) == static_cast<std::size_t>(pathLength));
        }
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
        // Only direct entries in this uniquely created directory are removed. No recursive cleanup.
        for (const auto& entry : directory.findChildFiles(juce::File::findFilesAndDirectories, false))
            if (entry.getParentDirectory() == directory && ! entry.isSymbolicLink())
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

ProjectDocument exampleDocument()
{
    ProjectDocument document;
    document.durationSeconds = 60.0;
    document.events = {{0.125, {0x90, 60, 100}}, {0.875, {0x80, 60, 0}},
                       {59.0, {0x90, 67, 80}}, {59.75, {0x80, 67, 0}}};
    return document;
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

template <typename T>
const ProjectFileError& fileError(const ProjectFileResult<T>& result)
{
    REQUIRE(std::holds_alternative<ProjectFileError>(result));
    return std::get<ProjectFileError>(result);
}

PreparedProjectSave prepare(const juce::File& file, const ProjectDocument& document)
{
    auto result = PreparedProjectSave::prepare(file, document);
    REQUIRE(std::holds_alternative<PreparedProjectSave>(result));
    return std::move(std::get<PreparedProjectSave>(result));
}

void requireLoaded(const juce::File& file, const ProjectDocument& expected)
{
    auto result = loadProjectFile(file);
    REQUIRE(std::holds_alternative<ProjectDocument>(result));
    CHECK(std::get<ProjectDocument>(result) == expected);
}

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
}

TEST_CASE("Project files save, replace and reopen exact document values", "[project][files]")
{
    const ScratchDirectory scratch;
    const auto target = scratch.file("performance.composer");
    const auto document = exampleDocument();
    auto first = saveProjectFile(target, document);
    REQUIRE(std::holds_alternative<SaveReceipt>(first));
    CHECK(std::get<SaveReceipt>(first).target == target);
    CHECK(std::get<SaveReceipt>(first).retainedPreviousVersion == juce::File());
    requireLoaded(target, document);
    const auto originalBytes = target.loadFileAsString();

    auto same = saveProjectFile(target, document);
    REQUIRE(std::holds_alternative<SaveReceipt>(same));
    CHECK(target.loadFileAsString() == originalBytes);
    requireLoaded(target, document);

    auto changed = document;
    changed.patch.gainDb = -18.0;
    changed.events[0].timeSeconds = 0.23456789123;
    auto replaced = saveProjectFile(target, changed);
    REQUIRE(std::holds_alternative<SaveReceipt>(replaced));
    CHECK(std::get<SaveReceipt>(replaced).retainedPreviousVersion == juce::File());
    requireLoaded(target, changed);
    CHECK(scratch.root().findChildFiles(juce::File::findFiles, false).size() == 1);
}

TEST_CASE("Invalid documents and targets do not create staging files or change prior bytes", "[project][files]")
{
    const ScratchDirectory scratch;
    const auto target = scratch.file("original.composer");
    writeFixture(target, "untouched synthetic original");
    auto invalid = exampleDocument();
    invalid.durationSeconds = std::numeric_limits<double>::quiet_NaN();
    auto failed = PreparedProjectSave::prepare(target, invalid);
    CHECK(fileError(failed).code == ProjectFileErrorCode::invalidDocument);
    CHECK(target.loadFileAsString() == "untouched synthetic original");
    CHECK(scratch.root().findChildFiles(juce::File::findFilesAndDirectories, false).size() == 1);

    const auto document = exampleDocument();
    CHECK(fileError(PreparedProjectSave::prepare(juce::File(), document)).code == ProjectFileErrorCode::invalidTarget);
    CHECK(fileError(PreparedProjectSave::prepare(scratch.root(), document)).code == ProjectFileErrorCode::invalidTarget);
    CHECK(fileError(PreparedProjectSave::prepare(scratch.root().getChildFile("absent/project.composer"), document)).code
          == ProjectFileErrorCode::invalidTarget);
    CHECK(fileError(PreparedProjectSave::prepare(scratch.file("NUL.composer"), document)).code == ProjectFileErrorCode::invalidTarget);
    CHECK(scratch.root().findChildFiles(juce::File::findFilesAndDirectories, false).size() == 1);
}

TEST_CASE("Project loads reject missing, empty, malformed, oversized and nonregular files", "[project][files]")
{
    const ScratchDirectory scratch;
    CHECK(fileError(loadProjectFile(scratch.file("missing.composer"))).code == ProjectFileErrorCode::missingFile);
    CHECK(fileError(loadProjectFile(scratch.root())).code == ProjectFileErrorCode::invalidTarget);

    const auto empty = scratch.file("empty.composer");
    writeFixture(empty, "");
    CHECK(fileError(loadProjectFile(empty)).code == ProjectFileErrorCode::emptyFile);
    CHECK(empty.getSize() == 0);

    const auto malformed = scratch.file("malformed.composer");
    writeFixture(malformed, "{not a project");
    CHECK(fileError(loadProjectFile(malformed)).code == ProjectFileErrorCode::invalidDocument);
    CHECK(malformed.loadFileAsString() == "{not a project");

    const auto oversized = scratch.file("oversized.composer");
    {
        juce::FileOutputStream stream(oversized, 0);
        REQUIRE(stream.openedOk());
        REQUIRE(stream.setPosition(static_cast<juce::int64>(maxProjectJsonBytes) + 1));
        REQUIRE(stream.truncate().wasOk());
        stream.flush();
        REQUIRE(stream.getStatus().wasOk());
    }
    CHECK(fileError(loadProjectFile(oversized)).code == ProjectFileErrorCode::oversizedFile);
    CHECK(oversized.getSize() == static_cast<juce::int64>(maxProjectJsonBytes) + 1);
}

TEST_CASE("A deny-delete lock on prepared bytes fails replacement without losing the original", "[project][files]")
{
    const ScratchDirectory scratch;
    const auto target = scratch.file("locked.composer");
    const auto original = exampleDocument();
    REQUIRE(std::holds_alternative<SaveReceipt>(saveProjectFile(target, original)));
    const auto originalBytes = target.loadFileAsString();
    auto changed = original;
    changed.patch.gainDb = -24.0;
    juce::File recovery;
    {
        auto prepared = prepare(target, changed);
        const auto stage = prepared.stagedFile();
        {
            const DenyDeleteReadHandle denyDelete(stage);
            auto failed = prepared.commit();
            CHECK(fileError(failed).code == ProjectFileErrorCode::commitFailed);
            recovery = fileError(failed).recoveryFile;
            CHECK(target.existsAsFile());
            CHECK(target.loadFileAsString() == originalBytes);
            CHECK(stage.existsAsFile());
            CHECK(recovery.existsAsFile());
        }
        CHECK(fileError(prepared.commit()).code == ProjectFileErrorCode::alreadyCommitted);
    }
    CHECK(recovery.existsAsFile());
    requireLoaded(target, original);
    REQUIRE(std::holds_alternative<SaveReceipt>(saveProjectFile(target, changed)));
    requireLoaded(target, changed);
}

TEST_CASE("Prepared saves reject target changes, creation and deletion", "[project][files]")
{
    const ScratchDirectory scratch;
    const auto target = scratch.file("external.composer");
    const auto document = exampleDocument();

    SECTION("Changed existing contents")
    {
        REQUIRE(std::holds_alternative<SaveReceipt>(saveProjectFile(target, document)));
        auto prepared = prepare(target, document);
        writeFixture(target, "externally changed synthetic bytes");
        CHECK(fileError(prepared.commit()).code == ProjectFileErrorCode::targetChanged);
        CHECK(target.loadFileAsString() == "externally changed synthetic bytes");
        CHECK(fileError(prepared.commit()).code == ProjectFileErrorCode::alreadyCommitted);
    }
    SECTION("Created previously absent destination")
    {
        auto prepared = prepare(target, document);
        writeFixture(target, "a concurrently created synthetic file");
        CHECK(fileError(prepared.commit()).code == ProjectFileErrorCode::targetChanged);
        CHECK(target.loadFileAsString() == "a concurrently created synthetic file");
    }
    SECTION("Deleted existing destination")
    {
        REQUIRE(std::holds_alternative<SaveReceipt>(saveProjectFile(target, document)));
        auto prepared = prepare(target, document);
        REQUIRE(target.deleteFile());
        CHECK(fileError(prepared.commit()).code == ProjectFileErrorCode::targetChanged);
        CHECK_FALSE(target.exists());
    }
}

TEST_CASE("Prepared save ownership is move-only, one-shot and cleans abandoned staging files", "[project][files]")
{
    const ScratchDirectory scratch;
    const auto document = exampleDocument();
    const auto firstTarget = scratch.file("first.composer");
    juce::File abandoned;
    {
        auto unused = prepare(firstTarget, document);
        abandoned = unused.stagedFile();
        REQUIRE(abandoned.existsAsFile());
    }
    CHECK_FALSE(abandoned.exists());
    CHECK_FALSE(firstTarget.exists());

    auto original = prepare(firstTarget, document);
    auto moved = std::move(original);
    CHECK(original.stagedFile() == juce::File());
    CHECK(fileError(original.commit()).code == ProjectFileErrorCode::alreadyCommitted);
    REQUIRE(std::holds_alternative<SaveReceipt>(moved.commit()));
    CHECK(fileError(moved.commit()).code == ProjectFileErrorCode::alreadyCommitted);
    requireLoaded(firstTarget, document);

    auto destination = prepare(scratch.file("abandoned.composer"), document);
    const auto abandonedByMove = destination.stagedFile();
    auto source = prepare(scratch.file("moved.composer"), document);
    destination = std::move(source);
    CHECK_FALSE(abandonedByMove.exists());
    CHECK(fileError(source.commit()).code == ProjectFileErrorCode::alreadyCommitted);
    REQUIRE(std::holds_alternative<SaveReceipt>(destination.commit()));
    requireLoaded(scratch.file("moved.composer"), document);
}

TEST_CASE("Tampering with staged bytes cannot replace a valid project", "[project][files]")
{
    const ScratchDirectory scratch;
    const auto target = scratch.file("protected.composer");
    const auto document = exampleDocument();
    REQUIRE(std::holds_alternative<SaveReceipt>(saveProjectFile(target, document)));
    const auto before = target.loadFileAsString();
    auto prepared = prepare(target, document);
    writeFixture(prepared.stagedFile(), "unexpected staged bytes");
    CHECK(fileError(prepared.commit()).code == ProjectFileErrorCode::verificationFailed);
    CHECK(target.loadFileAsString() == before);
    requireLoaded(target, document);
}

TEST_CASE("Long project directories allow both the first save and replacement", "[project][files][paths]")
{
    for (const int length : {207, 208, 212, 213, 238})
    {
        INFO("directory UTF-16 length " << length);
        const ScratchDirectory scratch(length);
        const auto target = scratch.file("song.composer");
        const auto original = exampleDocument();
        REQUIRE(std::holds_alternative<SaveReceipt>(saveProjectFile(target, original)));
        requireLoaded(target, original);
        auto changed = original;
        changed.patch.gainDb = -24.0;
        REQUIRE(std::holds_alternative<SaveReceipt>(saveProjectFile(target, changed)));
        requireLoaded(target, changed);
        CHECK(scratch.root().findChildFiles(juce::File::findFiles, false).size() == 1);
    }
}

TEST_CASE("A directory without room for both save siblings fails before creating files", "[project][files][paths]")
{
    const ScratchDirectory scratch(239);
    const auto target = scratch.file("song.composer");
    const auto document = exampleDocument();
    CHECK(fileError(PreparedProjectSave::prepare(target, document)).code == ProjectFileErrorCode::invalidTarget);
    CHECK_FALSE(target.exists());
    CHECK(scratch.root().findChildFiles(juce::File::findFiles, false).isEmpty());

    // A project written elsewhere can still be loaded here; the rejected save
    // leaves it and the directory unchanged instead of reserving unusable files.
    const auto encoded = encodeProject(document);
    REQUIRE(std::holds_alternative<std::string>(encoded));
    const auto& bytes = std::get<std::string>(encoded);
    writeFixture(target, bytes);
    requireLoaded(target, document);
    auto changed = document;
    changed.patch.gainDb = -24.0;
    CHECK(fileError(PreparedProjectSave::prepare(target, changed)).code == ProjectFileErrorCode::invalidTarget);
    CHECK(target.loadFileAsString().toStdString() == bytes);
    CHECK(scratch.root().findChildFiles(juce::File::findFiles, false).size() == 1);
}
#else
TEST_CASE("Project file storage reports unsupported platforms", "[project][files]")
{
    auto result = loadProjectFile(juce::File());
    REQUIRE(std::holds_alternative<ProjectFileError>(result));
    CHECK(std::get<ProjectFileError>(result).code == ProjectFileErrorCode::unsupportedPlatform);
}
#endif
