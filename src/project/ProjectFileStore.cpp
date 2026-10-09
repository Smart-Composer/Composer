#include <composer/project/ProjectFileStore.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <optional>
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

namespace composer::project
{
namespace
{
ProjectFileError failure(ProjectFileErrorCode code, std::string message, juce::File recovery = {})
{
    return {code, std::move(message), std::move(recovery)};
}

ProjectFileError documentFailure(const ProjectError& error)
{
    return failure(ProjectFileErrorCode::invalidDocument,
                   error.field.empty() ? error.message : error.field + ": " + error.message);
}

#if JUCE_WINDOWS
enum class FilePresence { absent, regular };

ProjectFileResult<FilePresence> inspect(const juce::File& file)
{
    const auto attributes = GetFileAttributesW(file.getFullPathName().toWideCharPointer());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return FilePresence::absent;
        return failure(ProjectFileErrorCode::readFailed,
                       "Cannot inspect file (Windows error " + std::to_string(error) + ")");
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return failure(ProjectFileErrorCode::invalidTarget, "Expected a regular file, not a directory or link");
    return FilePresence::regular;
}

std::optional<ProjectFileError> validateTargetPath(const juce::File& target)
{
    const auto name = target.getFileName();
    if (target == juce::File() || name.isEmpty() || name == "." || name == ".."
        || name.containsAnyOf("<>:\"/\\|?*") || name.endsWithChar('.') || name.endsWithChar(' '))
        return failure(ProjectFileErrorCode::invalidTarget, "Expected a named project file");
    for (const auto character : name)
        if (character < 32)
            return failure(ProjectFileErrorCode::invalidTarget, "File name contains a control character");

    const auto stem = name.upToFirstOccurrenceOf(".", false, false).toUpperCase();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL"
        || ((stem.startsWith("COM") || stem.startsWith("LPT")) && stem.length() == 4
            && stem[3] >= '1' && stem[3] <= '9'))
        return failure(ProjectFileErrorCode::invalidTarget, "File name is reserved by Windows");
    if (! target.getParentDirectory().isDirectory())
        return failure(ProjectFileErrorCode::invalidTarget, "The project directory does not exist");
    return std::nullopt;
}

ProjectFileResult<std::string> readBytes(const juce::File& file, bool allowEmpty)
{
    auto presence = inspect(file);
    if (const auto* error = std::get_if<ProjectFileError>(&presence))
        return *error;
    if (std::get<FilePresence>(presence) == FilePresence::absent)
        return failure(ProjectFileErrorCode::missingFile, "The project file does not exist");

    juce::FileInputStream input(file);
    if (! input.openedOk())
        return failure(ProjectFileErrorCode::readFailed, "Cannot open project file: " + input.getStatus().getErrorMessage().toStdString());
    const auto size = input.getTotalLength();
    if (size < 0)
        return failure(ProjectFileErrorCode::readFailed, "Cannot determine the project file size");
    if (static_cast<juce::uint64>(size) > static_cast<juce::uint64>(maxProjectJsonBytes))
        return failure(ProjectFileErrorCode::oversizedFile, "The project file exceeds the size limit");
    if (size == 0 && ! allowEmpty)
        return failure(ProjectFileErrorCode::emptyFile, "The project file is empty");

    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto request = static_cast<int>(std::min<std::size_t>(bytes.size() - offset, 64 * 1024));
        const auto received = input.read(bytes.data() + offset, request);
        if (received <= 0)
            return failure(ProjectFileErrorCode::readFailed, "The project file changed or could not be fully read");
        offset += static_cast<std::size_t>(received);
    }
    char extra = 0;
    if (input.read(&extra, 1) != 0 || input.getStatus().failed())
        return failure(ProjectFileErrorCode::readFailed, "The project file changed or could not be fully read");
    return bytes;
}

juce::File sibling(const juce::File& target, const char* suffix)
{
    return target.getSiblingFile(juce::Uuid().toString().substring(0, 16) + suffix);
}

bool fitsWindowsPath(const juce::File& file)
{
    // These executables use ordinary Win32 paths. Count UTF-16 code units,
    // including room for the terminating NUL, rather than Unicode characters.
    return std::wcslen(file.getFullPathName().toWideCharPointer()) < MAX_PATH;
}

std::optional<ProjectFileError> writeStagingFile(const juce::File& file, const std::string& bytes)
{
    juce::FileOutputStream output(file, 0);
    if (! output.openedOk())
        return failure(ProjectFileErrorCode::writeFailed, "Cannot open the staged project file");
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto count = std::min<std::size_t>(bytes.size() - offset, 64 * 1024);
        if (! output.write(bytes.data() + offset, count))
            return failure(ProjectFileErrorCode::writeFailed, "Could not completely write the staged project");
        offset += count;
    }
    output.flush();
    if (output.getStatus().failed())
        return failure(ProjectFileErrorCode::writeFailed,
                       "Could not flush the staged project: " + output.getStatus().getErrorMessage().toStdString());
    return std::nullopt;
}

bool hasExpectedBytes(const juce::File& file, const std::string& expected)
{
    auto actual = readBytes(file, true);
    const auto* bytes = std::get_if<std::string>(&actual);
    return bytes != nullptr && *bytes == expected;
}
#endif
}

struct PreparedProjectSave::Impl
{
    juce::File target;
    juce::File staged;
    juce::File backup;
    std::string encoded;
    std::optional<std::string> previous;
    bool ownsStage = false;
    bool preserveStage = false;
    bool attempted = false;

    ~Impl()
    {
        if (ownsStage && ! preserveStage && staged.existsAsFile() && ! staged.isSymbolicLink())
            staged.deleteFile();
    }

    juce::File recoveryFile() const
    {
        if (backup.existsAsFile())
            return backup;
        if (staged.existsAsFile())
            return staged;
        return {};
    }
};

PreparedProjectSave::PreparedProjectSave(std::unique_ptr<Impl> implementation)
    : impl(std::move(implementation)) {}
PreparedProjectSave::PreparedProjectSave(PreparedProjectSave&&) noexcept = default;
PreparedProjectSave& PreparedProjectSave::operator=(PreparedProjectSave&&) noexcept = default;
PreparedProjectSave::~PreparedProjectSave() = default;

juce::File PreparedProjectSave::stagedFile() const
{
    return impl != nullptr ? impl->staged : juce::File();
}

ProjectFileResult<ProjectDocument> loadProjectFile(const juce::File& file)
{
#if JUCE_WINDOWS
    if (auto error = validateTargetPath(file))
        return *error;
    auto bytes = readBytes(file, false);
    if (const auto* error = std::get_if<ProjectFileError>(&bytes))
        return *error;
    auto document = decodeProject(std::get<std::string>(bytes));
    if (const auto* error = std::get_if<ProjectError>(&document))
        return documentFailure(*error);
    return std::get<ProjectDocument>(std::move(document));
#else
    juce::ignoreUnused(file);
    return failure(ProjectFileErrorCode::unsupportedPlatform, "Project file storage is supported on Windows");
#endif
}

ProjectFileResult<PreparedProjectSave> PreparedProjectSave::prepare(
    const juce::File& target, const ProjectDocument& document)
{
    auto encoded = encodeProject(document);
    if (const auto* error = std::get_if<ProjectError>(&encoded))
        return documentFailure(*error);
#if JUCE_WINDOWS
    if (auto error = validateTargetPath(target))
        return *error;
    auto presence = inspect(target);
    if (const auto* error = std::get_if<ProjectFileError>(&presence))
        return *error;
    auto state = std::make_unique<Impl>();
    state->target = target;
    state->encoded = std::get<std::string>(std::move(encoded));
    state->staged = sibling(target, ".tmp");
    state->backup = sibling(target, ".bak");
    if (!fitsWindowsPath(state->target) || !fitsWindowsPath(state->staged)
        || !fitsWindowsPath(state->backup))
        return failure(ProjectFileErrorCode::invalidTarget,
                       "The project directory is too long for safe temporary and backup files; choose a shorter path");
    if (std::get<FilePresence>(presence) == FilePresence::regular)
    {
        auto previous = readBytes(target, true);
        if (const auto* error = std::get_if<ProjectFileError>(&previous))
            return *error;
        state->previous = std::get<std::string>(std::move(previous));
    }

    const auto reservation = CreateFileW(state->staged.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                                          FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reservation == INVALID_HANDLE_VALUE)
        return failure(ProjectFileErrorCode::writeFailed, "Cannot create a unique sibling staging file");
    state->ownsStage = true;
    if (! CloseHandle(reservation))
        return failure(ProjectFileErrorCode::writeFailed, "Cannot close the staging-file reservation");
    if (auto error = writeStagingFile(state->staged, state->encoded))
        return *error;
    if (! hasExpectedBytes(state->staged, state->encoded))
        return failure(ProjectFileErrorCode::verificationFailed, "The staged project did not verify after writing");
    return PreparedProjectSave(std::move(state));
#else
    juce::ignoreUnused(target);
    return failure(ProjectFileErrorCode::unsupportedPlatform, "Project file storage is supported on Windows");
#endif
}

ProjectFileResult<SaveReceipt> PreparedProjectSave::commit()
{
    if (impl == nullptr || impl->attempted)
        return failure(ProjectFileErrorCode::alreadyCommitted, "This prepared save has already been consumed");
    impl->attempted = true;
#if JUCE_WINDOWS
    if (! hasExpectedBytes(impl->staged, impl->encoded))
    {
        impl->preserveStage = true;
        return failure(ProjectFileErrorCode::verificationFailed, "The staged project changed before saving", impl->recoveryFile());
    }
    auto current = inspect(impl->target);
    const auto* presence = std::get_if<FilePresence>(&current);
    const bool same = presence != nullptr
        && (impl->previous.has_value()
                ? *presence == FilePresence::regular && hasExpectedBytes(impl->target, *impl->previous)
                : *presence == FilePresence::absent);
    if (! same)
        return failure(ProjectFileErrorCode::targetChanged, "The destination changed or cannot be verified; prepare a new save");

    BOOL succeeded = FALSE;
    if (impl->previous.has_value())
    {
        auto backupState = inspect(impl->backup);
        const auto* backupPresence = std::get_if<FilePresence>(&backupState);
        if (backupPresence == nullptr || *backupPresence != FilePresence::absent)
            return failure(ProjectFileErrorCode::commitFailed, "Cannot choose a unique previous-version filename");
        succeeded = ReplaceFileW(impl->target.getFullPathName().toWideCharPointer(),
                                 impl->staged.getFullPathName().toWideCharPointer(),
                                 impl->backup.getFullPathName().toWideCharPointer(), 0, nullptr, nullptr);
    }
    else
    {
        succeeded = MoveFileExW(impl->staged.getFullPathName().toWideCharPointer(),
                                impl->target.getFullPathName().toWideCharPointer(), MOVEFILE_WRITE_THROUGH);
    }
    if (! succeeded)
    {
        const auto error = GetLastError();
        impl->preserveStage = true;
        return failure(ProjectFileErrorCode::commitFailed,
                       "Could not replace the project (Windows error " + std::to_string(error)
                           + "); retained files may be used for recovery", impl->recoveryFile());
    }
    impl->ownsStage = false;
    if (! hasExpectedBytes(impl->target, impl->encoded))
        return failure(ProjectFileErrorCode::verificationFailed,
                       "The saved project could not be verified; keep the retained previous version", impl->recoveryFile());

    juce::File retained;
    if (impl->previous.has_value())
    {
        if (! hasExpectedBytes(impl->backup, *impl->previous))
            return failure(ProjectFileErrorCode::verificationFailed,
                           "Saved bytes verified, but the previous-version backup could not be verified",
                           impl->recoveryFile());
        if (! impl->backup.deleteFile())
            retained = impl->backup;
    }
    return SaveReceipt{impl->target, retained};
#else
    return failure(ProjectFileErrorCode::unsupportedPlatform, "Project file storage is supported on Windows");
#endif
}

ProjectFileResult<SaveReceipt> saveProjectFile(const juce::File& target, const ProjectDocument& document)
{
    auto prepared = PreparedProjectSave::prepare(target, document);
    if (const auto* error = std::get_if<ProjectFileError>(&prepared))
        return *error;
    return std::get<PreparedProjectSave>(prepared).commit();
}
}
