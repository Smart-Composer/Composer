#pragma once

#include "InstrumentPatch.h"

#include <span>
#include <utility>
#include <vector>

namespace composer::contracts
{
inline constexpr std::uint32_t commandSchemaVersion = 1;
inline constexpr std::uint64_t maximumProjectRevision = 9007199254740991ULL;

struct ProjectCommand
{
    std::string projectInstanceId;
    std::uint64_t expectedRevision = 0;
    InstrumentPatch patch;
};

struct ProjectPatchSnapshot
{
    std::string projectInstanceId;
    std::uint64_t revision = 0;
    InstrumentPatch patch;
};

enum class EditOutcome { applied, noChange };

std::optional<ContractError> validateCommand(const ProjectCommand& command);
Result<ProjectCommand> decodeCommand(std::string_view source);
Result<std::string> encodeCommand(const ProjectCommand& command);

// One editing thread owns a session. Publish prepared patch snapshots separately.
class ProjectPatchSession
{
public:
    ProjectPatchSession(const ProjectPatchSession&) = delete;
    ProjectPatchSession& operator=(const ProjectPatchSession&) = delete;
    ProjectPatchSession(ProjectPatchSession&&) noexcept = default;
    ProjectPatchSession& operator=(ProjectPatchSession&&) noexcept = default;

    static Result<ProjectPatchSession> create(
        std::string freshInstanceId, InstrumentPatch initialPatch = {},
        std::uint64_t initialRevision = 0);

    const ProjectPatchSnapshot& snapshot() const noexcept { return state; }
    std::span<const InstrumentPatch> undoHistory() const noexcept { return undoPatches; }
    std::span<const InstrumentPatch> redoHistory() const noexcept { return redoPatches; }

    // Commit a prepared non-patch context change on the editing thread.
    // Advance revision only; preserve token, patch and both patch-history stacks.
    Result<EditOutcome> advanceContext();

    Result<EditOutcome> apply(const ProjectCommand& command);
    Result<EditOutcome> undo();
    Result<EditOutcome> redo();

private:
    explicit ProjectPatchSession(ProjectPatchSnapshot snapshot) : state(std::move(snapshot)) {}

    ProjectPatchSnapshot state;
    std::vector<InstrumentPatch> undoPatches;
    std::vector<InstrumentPatch> redoPatches;
};
}
