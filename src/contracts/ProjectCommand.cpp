#include <composer/contracts/ProjectCommand.h>
#include "JsonContract.h"

#include <algorithm>
#include <new>
#include <utility>

namespace composer::contracts
{
namespace
{
bool validInstanceId(std::string_view value)
{
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

ContractError invalidCommand(std::string field, std::string message)
{
    return {ErrorCode::invalidCommand, std::move(field), std::move(message)};
}

ContractError nestedPatchError(const ContractError& error)
{
    return {error.code == ErrorCode::unsupportedVersion ? error.code : ErrorCode::invalidCommand,
            error.field.empty() ? "patch" : "patch." + error.field, error.message};
}

ContractError exhaustedRevision()
{
    return {ErrorCode::revisionExhausted, "expected_revision", "Project revision cannot advance further"};
}
}

std::optional<ContractError> validateCommand(const ProjectCommand& command)
{
    if (!validInstanceId(command.projectInstanceId))
        return invalidCommand("project_instance_id", "Expected an ASCII identifier of 1 to 128 letters, digits, hyphens or underscores");
    if (command.expectedRevision > maximumProjectRevision)
        return invalidCommand("expected_revision", "Project revision is outside the supported range");
    if (auto error = validatePatch(command.patch))
        return nestedPatchError(*error);
    return std::nullopt;
}

Result<ProjectCommand> decodeCommand(std::string_view source)
{
    auto allocationStage = ErrorCode::invalidJson;
    try
    {
        auto parsed = detail::parseJson(source);
        if (const auto* error = std::get_if<ContractError>(&parsed))
            return *error;
        allocationStage = ErrorCode::invalidCommand;
        const auto& value = std::get<detail::Json>(parsed);
        try
        {
            if (auto error = detail::checkVersion(value, commandSchemaVersion, ErrorCode::invalidCommand))
                return *error;
            for (const auto* field : {"type", "project_instance_id", "expected_revision", "patch"})
                if (!value.contains(field))
                    return invalidCommand(field, "Missing command field");
            if (value.size() != 5)
                return invalidCommand({}, "Expected only the declared command fields");
            if (value["type"] != "replace_instrument_patch")
                return invalidCommand("type", "Expected a patch-replacement command type");
            if (!value["project_instance_id"].is_string()
                || !validInstanceId(value["project_instance_id"].get_ref<const std::string&>()))
                return invalidCommand("project_instance_id", "Expected an ASCII identifier of 1 to 128 letters, digits, hyphens or underscores");
            if (!detail::isUnsignedInteger(value["expected_revision"], maximumProjectRevision))
                return invalidCommand("expected_revision", "Expected an integer project revision inside the supported range");
            auto patch = detail::patchFromJson(value["patch"]);
            if (const auto* error = std::get_if<ContractError>(&patch))
                return nestedPatchError(*error);
            ProjectCommand command{value["project_instance_id"].get<std::string>(),
                                   value["expected_revision"].get<std::uint64_t>(),
                                   std::get<InstrumentPatch>(patch)};
            if (auto error = validateCommand(command))
                return *error;
            return command;
        }
        catch (const detail::Json::exception&)
        {
            return invalidCommand({}, "Invalid command value");
        }
    }
    catch (const std::bad_alloc&)
    {
        return ContractError{allocationStage, {}, {}};
    }
}

Result<std::string> encodeCommand(const ProjectCommand& command)
{
    try
    {
        if (auto error = validateCommand(command))
            return *error;
        try
        {
            const detail::Json value = {
                {"schema_version", commandSchemaVersion}, {"type", "replace_instrument_patch"},
                {"project_instance_id", command.projectInstanceId}, {"expected_revision", command.expectedRevision},
                {"patch", detail::patchToJson(command.patch)}
            };
            return value.dump();
        }
        catch (const detail::Json::exception&)
        {
            return invalidCommand({}, "Command serialization failed");
        }
    }
    catch (const std::bad_alloc&)
    {
        return ContractError{ErrorCode::invalidCommand, {}, {}};
    }
}

Result<ProjectPatchSession> ProjectPatchSession::create(
    std::string freshInstanceId, InstrumentPatch initialPatch, std::uint64_t initialRevision)
{
    if (auto error = validateCommand({freshInstanceId, initialRevision, initialPatch}))
        return *error;
    return ProjectPatchSession({std::move(freshInstanceId), initialRevision, initialPatch});
}

Result<EditOutcome> ProjectPatchSession::advanceContext()
{
    if (state.revision == maximumProjectRevision)
        return exhaustedRevision();
    ++state.revision;
    return EditOutcome::applied;
}

Result<EditOutcome> ProjectPatchSession::apply(const ProjectCommand& command)
{
    if (auto error = validateCommand(command))
        return *error;
    if (command.projectInstanceId != state.projectInstanceId)
        return ContractError{ErrorCode::wrongProject, "project_instance_id", "Command targets a different project instance"};
    if (command.expectedRevision != state.revision)
        return ContractError{ErrorCode::staleRevision, "expected_revision", "Command targets an obsolete project revision"};
    if (command.patch == state.patch)
        return EditOutcome::noChange;
    if (state.revision == maximumProjectRevision)
        return exhaustedRevision();

    undoPatches.push_back(state.patch);
    redoPatches.clear();
    state.patch = command.patch;
    ++state.revision;
    return EditOutcome::applied;
}

Result<EditOutcome> ProjectPatchSession::undo()
{
    if (undoPatches.empty())
        return EditOutcome::noChange;
    if (state.revision == maximumProjectRevision)
        return exhaustedRevision();
    redoPatches.push_back(state.patch);
    state.patch = undoPatches.back();
    undoPatches.pop_back();
    ++state.revision;
    return EditOutcome::applied;
}

Result<EditOutcome> ProjectPatchSession::redo()
{
    if (redoPatches.empty())
        return EditOutcome::noChange;
    if (state.revision == maximumProjectRevision)
        return exhaustedRevision();
    undoPatches.push_back(state.patch);
    state.patch = redoPatches.back();
    redoPatches.pop_back();
    ++state.revision;
    return EditOutcome::applied;
}
}
