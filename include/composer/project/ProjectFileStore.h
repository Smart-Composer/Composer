#pragma once

#include "ProjectDocument.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <string>
#include <variant>

namespace composer::project
{
enum class ProjectFileErrorCode
{
    invalidDocument,
    invalidTarget,
    missingFile,
    emptyFile,
    oversizedFile,
    readFailed,
    writeFailed,
    targetChanged,
    alreadyCommitted,
    commitFailed,
    verificationFailed,
    unsupportedPlatform
};

struct ProjectFileError
{
    ProjectFileErrorCode code;
    std::string message;
    juce::File recoveryFile;
};

template <typename Value>
using ProjectFileResult = std::variant<Value, ProjectFileError>;

struct SaveReceipt
{
    juce::File target;
    juce::File retainedPreviousVersion;
};

// All file operations, including destruction of prepared saves, belong off the audio thread.
ProjectFileResult<ProjectDocument> loadProjectFile(const juce::File& file);

class PreparedProjectSave
{
public:
    PreparedProjectSave(const PreparedProjectSave&) = delete;
    PreparedProjectSave& operator=(const PreparedProjectSave&) = delete;
    PreparedProjectSave(PreparedProjectSave&&) noexcept;
    PreparedProjectSave& operator=(PreparedProjectSave&&) noexcept;
    ~PreparedProjectSave();

    static ProjectFileResult<PreparedProjectSave> prepare(
        const juce::File& target, const ProjectDocument& document);

    // Exposes the prepared path for diagnostics and file-sharing checks, not for editing.
    juce::File stagedFile() const;

    // The first call consumes this save attempt, whether it succeeds or fails.
    ProjectFileResult<SaveReceipt> commit();

private:
    struct Impl;
    explicit PreparedProjectSave(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> impl;
};

ProjectFileResult<SaveReceipt> saveProjectFile(
    const juce::File& target, const ProjectDocument& document);
}
