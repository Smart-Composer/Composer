#pragma once

#include <composer/contracts/InstrumentPatch.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace composer::project
{
inline constexpr std::uint32_t projectSchemaVersion = 1;
inline constexpr double projectTempoBpm = 120.0;
inline constexpr double maxProjectDurationSeconds = 24.0 * 60.0 * 60.0;
inline constexpr std::size_t maxProjectEvents = 1'000'000;
inline constexpr std::size_t maxProjectJsonBytes = 128 * 1024 * 1024;

struct MidiEvent
{
    double timeSeconds = 0.0;
    std::vector<std::uint8_t> bytes;

    bool operator==(const MidiEvent&) const = default;
};

struct ProjectDocument
{
    contracts::InstrumentPatch patch;
    double durationSeconds = 0.0;
    std::vector<MidiEvent> events;

    bool operator==(const ProjectDocument&) const = default;
};

enum class ProjectErrorCode
{
    invalidJson,
    unsupportedVersion,
    invalidDocument,
    invalidPatch,
    resourceLimit
};

struct ProjectError
{
    ProjectErrorCode code;
    std::string field;
    std::string message;

    bool operator==(const ProjectError&) const = default;
};

template <typename Value>
using ProjectResult = std::variant<Value, ProjectError>;

// Project operations allocate memory and belong outside audio processing.
std::optional<ProjectError> validateProject(const ProjectDocument& document);
ProjectResult<ProjectDocument> decodeProject(std::string_view source);
ProjectResult<std::string> encodeProject(const ProjectDocument& document);
}
