#include "JsonContract.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <vector>

namespace composer::contracts
{
namespace
{
ContractError invalidPatch(std::string field, std::string message)
{
    return {ErrorCode::invalidPatch, std::move(field), std::move(message)};
}
}

std::optional<ContractError> validatePatch(const InstrumentPatch& patch)
{
    if (std::none_of(waveformDescriptors.begin(), waveformDescriptors.end(),
                     [&](const auto& choice) { return choice.value == patch.waveform; }))
        return invalidPatch("waveform", "Unknown oscillator waveform");

    for (const auto& descriptor : parameterDescriptors)
    {
        if (descriptor.member == nullptr)
            continue;
        const auto value = patch.*descriptor.member;
        if (!std::isfinite(value) || value < descriptor.minimum || value > descriptor.maximum)
            return invalidPatch(std::string(descriptor.id), "Value must be finite and inside the parameter bounds");
    }
    return std::nullopt;
}

namespace detail
{
Result<Json> parseJson(std::string_view source)
{
    // The JSON lexer treats a literal NUL as end-of-input, even inside an iterator range.
    if (source.find('\0') != std::string_view::npos)
        return ContractError{ErrorCode::invalidJson, {}, "Literal NUL is not valid JSON"};
    try
    {
        std::vector<std::set<std::string>> members;
        const auto callback = [&](int, Json::parse_event_t event, Json& value)
        {
            if (event == Json::parse_event_t::object_start)
                members.emplace_back();
            else if (event == Json::parse_event_t::key)
            {
                if (members.empty() || !members.back().insert(value.get<std::string>()).second)
                    throw std::invalid_argument("Duplicate JSON member");
            }
            else if (event == Json::parse_event_t::object_end)
                members.pop_back();
            return true;
        };
        return Json::parse(source.begin(), source.end(), callback);
    }
    catch (const std::exception&)
    {
        return ContractError{ErrorCode::invalidJson, {}, "Malformed, duplicate-member or nonfinite JSON input"};
    }
}

bool isUnsignedInteger(const Json& value, std::uint64_t maximum)
{
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() <= maximum;
    if (value.is_number_integer())
    {
        const auto signedValue = value.get<std::int64_t>();
        return signedValue >= 0 && static_cast<std::uint64_t>(signedValue) <= maximum;
    }
    return false;
}

std::optional<ContractError> checkVersion(const Json& value, std::uint32_t version, ErrorCode invalidCode)
{
    if (!value.is_object() || !value.contains("schema_version")
        || !isUnsignedInteger(value["schema_version"], UINT32_MAX))
        return ContractError{invalidCode, "schema_version", "Expected an integer schema version"};
    if (value["schema_version"].get<std::uint32_t>() != version)
        return ContractError{ErrorCode::unsupportedVersion, "schema_version", "Unsupported schema version"};
    return std::nullopt;
}

Result<InstrumentPatch> patchFromJson(const Json& value)
{
    if (auto error = checkVersion(value, patchSchemaVersion, ErrorCode::invalidPatch))
        return *error;
    if (value.size() != parameterDescriptors.size() + 1)
        return invalidPatch({}, "Expected exactly the version and declared patch parameters");

    InstrumentPatch patch;
    for (const auto& descriptor : parameterDescriptors)
    {
        const auto key = std::string(descriptor.id);
        if (!value.contains(key))
            return invalidPatch(key, "Missing patch parameter");
        if (descriptor.kind == ParameterKind::choice)
        {
            if (!value[key].is_string())
                return invalidPatch(key, "Expected a waveform string");
            const auto choice = value[key].get<std::string>();
            const auto found = std::find_if(waveformDescriptors.begin(), waveformDescriptors.end(),
                                           [&](const auto& item) { return item.id == choice; });
            if (found == waveformDescriptors.end())
                return invalidPatch(key, "Unknown oscillator waveform");
            patch.waveform = found->value;
        }
        else
        {
            if (!value[key].is_number())
                return invalidPatch(key, "Expected a number");
            patch.*descriptor.member = value[key].get<double>();
        }
    }
    if (auto error = validatePatch(patch))
        return *error;
    return patch;
}

Json patchToJson(const InstrumentPatch& patch)
{
    Json value = {{"schema_version", patchSchemaVersion}};
    value["waveform"] = waveformDescriptors[static_cast<std::size_t>(patch.waveform)].id;
    for (const auto& descriptor : parameterDescriptors)
    {
        if (descriptor.member != nullptr)
        {
            const auto number = patch.*descriptor.member;
            value[std::string(descriptor.id)] = number == 0.0 ? 0.0 : number;
        }
    }
    return value;
}
}

Result<InstrumentPatch> decodePatch(std::string_view source)
{
    auto parsed = detail::parseJson(source);
    if (const auto* error = std::get_if<ContractError>(&parsed))
        return *error;
    try
    {
        return detail::patchFromJson(std::get<detail::Json>(parsed));
    }
    catch (const detail::Json::exception&)
    {
        return invalidPatch({}, "Invalid patch value");
    }
}

Result<std::string> encodePatch(const InstrumentPatch& patch)
{
    if (auto error = validatePatch(patch))
        return *error;
    try
    {
        return detail::patchToJson(patch).dump();
    }
    catch (const detail::Json::exception&)
    {
        return invalidPatch({}, "Patch serialization failed");
    }
}
}
