#include <composer/project/ProjectDocument.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <new>
#include <utility>

namespace composer::project
{
namespace
{
using Json = nlohmann::json;

ProjectError invalid(std::string field, std::string message)
{
    return {ProjectErrorCode::invalidDocument, std::move(field), std::move(message)};
}

ProjectError patchError(const contracts::ContractError& error)
{
    return {error.code == contracts::ErrorCode::unsupportedVersion
                ? ProjectErrorCode::unsupportedVersion : ProjectErrorCode::invalidPatch,
            error.field.empty() ? "patch" : "patch." + error.field, error.message};
}

bool hasFields(const Json& value, std::initializer_list<const char*> names)
{
    return value.is_object() && value.size() == names.size()
        && std::all_of(names.begin(), names.end(), [&](const auto* name) { return value.contains(name); });
}

bool isUnsignedInteger(const Json& value, std::uint64_t maximum)
{
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() <= maximum;
    if (value.is_number_integer())
    {
        const auto number = value.get<std::int64_t>();
        return number >= 0 && static_cast<std::uint64_t>(number) <= maximum;
    }
    return false;
}

std::string eventField(std::size_t index)
{
    return "events[" + std::to_string(index) + "]";
}

std::optional<ProjectError> validateEvent(const MidiEvent& event, double previousTime,
                                          double duration, const std::string& field)
{
    if (!std::isfinite(event.timeSeconds) || event.timeSeconds < 0.0 || event.timeSeconds > duration)
        return invalid(field + ".time_seconds", "Event time must be finite and within the performance duration");
    if (event.timeSeconds < previousTime)
        return invalid(field + ".time_seconds", "Events must be ordered by nondecreasing time");
    if (event.bytes.empty() || event.bytes.front() < 0x80 || event.bytes.front() > 0xef)
        return invalid(field + ".bytes", "Expected a complete MIDI 1.0 channel message");
    const auto status = event.bytes.front() & 0xf0;
    const std::size_t length = status == 0xc0 || status == 0xd0 ? 2 : 3;
    if (event.bytes.size() != length)
        return invalid(field + ".bytes", "Message length does not match its MIDI status");
    if (std::any_of(event.bytes.begin() + 1, event.bytes.end(), [](auto byte) { return byte > 0x7f; }))
        return invalid(field + ".bytes", "MIDI data bytes must be seven-bit values");
    return std::nullopt;
}

// Complete each container once before moving it into its parent. The callback DOM
// parser scans the accumulated parent array at every object end, making event
// lists quadratic; this SAX builder only appends each completed event once.
class ProjectJsonBuilder : public nlohmann::json_sax<Json>
{
public:
    ProjectJsonBuilder() { containers.reserve(5); }

    bool null() override { return append(nullptr); }
    bool boolean(bool value) override { return append(value); }
    bool number_integer(number_integer_t value) override { return append(value); }
    bool number_unsigned(number_unsigned_t value) override { return append(value); }
    bool number_float(number_float_t value, const string_t&) override { return append(value); }
    bool string(string_t& value) override { return append(std::move(value)); }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return start(Json::object()); }
    bool start_array(std::size_t) override { return start(Json::array()); }
    bool end_object() override { return finish(); }
    bool end_array() override { return finish(); }

    bool key(string_t& value) override
    {
        checkDepth();
        auto& parent = containers.back();
        if (parent.value.contains(value))
            throw ProjectError{ProjectErrorCode::invalidJson, {}, "Duplicate JSON field"};
        parent.key = std::move(value);
        return true;
    }

    bool parse_error(std::size_t, const std::string&, const Json::exception&) override { return false; }

    Json takeResult() { return std::move(result); }

private:
    struct Container
    {
        Json value;
        std::string key;
    };
    std::vector<Container> containers;
    Json result;

    void checkDepth() const
    {
        // Match the JSON callback depth: root containers start at zero, and
        // version 1's innermost MIDI byte values are at depth four.
        if (containers.size() > 4)
            throw ProjectError{ProjectErrorCode::invalidDocument, {}, "JSON nesting exceeds the project structure"};
    }

    bool start(Json value)
    {
        checkDepth();
        containers.push_back({std::move(value), {}});
        return true;
    }

    bool finish()
    {
        auto value = std::move(containers.back().value);
        containers.pop_back();
        return append(std::move(value));
    }

    bool append(Json value)
    {
        checkDepth();
        if (containers.empty())
            result = std::move(value);
        else if (auto& parent = containers.back(); parent.value.is_object())
            parent.value.emplace(std::move(parent.key), std::move(value));
        else
            parent.value.push_back(std::move(value));
        return true;
    }
};

ProjectResult<Json> parseJson(std::string_view source)
{
    if (source.size() > maxProjectJsonBytes)
        return ProjectError{ProjectErrorCode::resourceLimit, {}, "Project JSON exceeds 128 MiB"};
    // The JSON lexer can treat literal NUL as end-of-input even with bounded iterators.
    if (source.find('\0') != std::string_view::npos)
        return ProjectError{ProjectErrorCode::invalidJson, {}, "Literal NUL is not valid JSON"};
    try
    {
        ProjectJsonBuilder builder;
        if (!Json::sax_parse(source.begin(), source.end(), &builder))
            return ProjectError{ProjectErrorCode::invalidJson, {}, "Malformed or nonfinite JSON input"};
        return builder.takeResult();
    }
    catch (const ProjectError& error)
    {
        return error;
    }
    catch (const Json::exception&)
    {
        return ProjectError{ProjectErrorCode::invalidJson, {}, "Malformed or nonfinite JSON input"};
    }
    catch (const std::bad_alloc&)
    {
        return ProjectError{ProjectErrorCode::resourceLimit, {}, "Insufficient memory for project JSON"};
    }
}
}

std::optional<ProjectError> validateProject(const ProjectDocument& document)
{
    if (auto error = contracts::validatePatch(document.patch))
        return patchError(*error);
    if (!std::isfinite(document.durationSeconds) || document.durationSeconds < 0.0
        || document.durationSeconds > maxProjectDurationSeconds)
        return invalid("duration_seconds", "Duration must be finite and between zero and 24 hours");
    if (document.events.size() > maxProjectEvents)
        return ProjectError{ProjectErrorCode::resourceLimit, "events", "A performance may contain at most 1000000 events"};
    double previousTime = 0.0;
    for (std::size_t index = 0; index < document.events.size(); ++index)
    {
        const auto& event = document.events[index];
        if (auto error = validateEvent(event, previousTime, document.durationSeconds, eventField(index)))
            return error;
        previousTime = event.timeSeconds;
    }
    return std::nullopt;
}

ProjectResult<ProjectDocument> decodeProject(std::string_view source)
{
    auto parsed = parseJson(source);
    if (const auto* error = std::get_if<ProjectError>(&parsed))
        return *error;
    try
    {
        const auto& value = std::get<Json>(parsed);
        if (!value.is_object() || !value.contains("schema_version")
            || !isUnsignedInteger(value["schema_version"], std::numeric_limits<std::uint64_t>::max()))
            return invalid("schema_version", "Expected an integer schema version");
        if (value["schema_version"].get<std::uint64_t>() != projectSchemaVersion)
            return ProjectError{ProjectErrorCode::unsupportedVersion, "schema_version", "Unsupported project schema version"};
        if (!hasFields(value, {"schema_version", "tempo_bpm", "patch", "duration_seconds", "events"}))
            return invalid({}, "Expected exactly the version, tempo, patch, duration and events fields");
        if (!value["tempo_bpm"].is_number() || value["tempo_bpm"].get<double>() != projectTempoBpm)
            return invalid("tempo_bpm", "Version 1 requires a fixed tempo of 120 BPM");
        if (!value["duration_seconds"].is_number())
            return invalid("duration_seconds", "Expected a number");
        if (!value["events"].is_array())
            return invalid("events", "Expected an ordered array of MIDI events");
        if (value["events"].size() > maxProjectEvents)
            return ProjectError{ProjectErrorCode::resourceLimit, "events", "A performance may contain at most 1000000 events"};

        // dump preserves JSON number categories (for example 1.0 remains floating point),
        // so the patch decoder still rejects noninteger version tokens.
        auto patch = contracts::decodePatch(value["patch"].dump());
        if (const auto* error = std::get_if<contracts::ContractError>(&patch))
            return patchError(*error);

        ProjectDocument document;
        document.patch = std::get<contracts::InstrumentPatch>(patch);
        document.durationSeconds = value["duration_seconds"].get<double>();
        document.events.reserve(value["events"].size());
        for (const auto& encoded : value["events"])
        {
            const auto field = eventField(document.events.size());
            if (!hasFields(encoded, {"time_seconds", "bytes"}))
                return invalid(field, "Expected exactly event time and bytes");
            if (!encoded["time_seconds"].is_number())
                return invalid(field + ".time_seconds", "Expected a number");
            const auto& bytes = encoded["bytes"];
            if (!bytes.is_array() || bytes.size() < 2 || bytes.size() > 3)
                return invalid(field + ".bytes", "Expected two or three MIDI bytes");
            MidiEvent event;
            event.timeSeconds = encoded["time_seconds"].get<double>();
            event.bytes.reserve(bytes.size());
            for (const auto& byte : bytes)
            {
                if (!isUnsignedInteger(byte, 0xff))
                    return invalid(field + ".bytes", "MIDI bytes must be integers between zero and 255");
                event.bytes.push_back(byte.get<std::uint8_t>());
            }
            document.events.push_back(std::move(event));
        }
        if (auto error = validateProject(document))
            return *error;
        return document;
    }
    catch (const Json::exception&)
    {
        return invalid({}, "Invalid project value");
    }
    catch (const std::bad_alloc&)
    {
        return ProjectError{ProjectErrorCode::resourceLimit, {}, "Insufficient memory for project data"};
    }
}

ProjectResult<std::string> encodeProject(const ProjectDocument& document)
{
    if (auto error = validateProject(document))
        return *error;
    try
    {
        auto patch = contracts::encodePatch(document.patch);
        if (const auto* error = std::get_if<contracts::ContractError>(&patch))
            return patchError(*error);
        Json value {{"schema_version", projectSchemaVersion}, {"tempo_bpm", projectTempoBpm},
                    {"patch", Json::parse(std::get<std::string>(patch))},
                    {"duration_seconds", document.durationSeconds}, {"events", Json::array()}};
        for (const auto& event : document.events)
            value["events"].push_back({{"time_seconds", event.timeSeconds}, {"bytes", event.bytes}});
        auto encoded = value.dump();
        if (encoded.size() > maxProjectJsonBytes)
            return ProjectError{ProjectErrorCode::resourceLimit, {}, "Project JSON exceeds 128 MiB"};
        return encoded;
    }
    catch (const Json::exception&)
    {
        return invalid({}, "Project serialization failed");
    }
    catch (const std::bad_alloc&)
    {
        return ProjectError{ProjectErrorCode::resourceLimit, {}, "Insufficient memory for project JSON"};
    }
}
}
