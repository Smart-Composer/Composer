#pragma once

#include <composer/contracts/InstrumentPatch.h>
#include <nlohmann/json.hpp>

namespace composer::contracts::detail
{
using Json = nlohmann::json;

Result<Json> parseJson(std::string_view source);
Result<InstrumentPatch> patchFromJson(const Json& value);
Json patchToJson(const InstrumentPatch& patch);
bool isUnsignedInteger(const Json& value, std::uint64_t maximum);
std::optional<ContractError> checkVersion(const Json& value, std::uint32_t version, ErrorCode invalidCode);
}
