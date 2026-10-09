#pragma once

#include <string>
#include <variant>

namespace composer::contracts
{
enum class ErrorCode
{
    invalidJson,
    unsupportedVersion,
    invalidPatch,
    invalidCommand,
    wrongProject,
    staleRevision,
    revisionExhausted
};

struct ContractError
{
    ErrorCode code;
    std::string field;
    std::string message;

    bool operator==(const ContractError&) const = default;
};

template <typename Value>
using Result = std::variant<Value, ContractError>;
}
