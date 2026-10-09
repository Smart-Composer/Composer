#include <composer/contracts/ProjectCommand.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace composer::contracts;

namespace
{
constexpr std::size_t maximumBytes = 64 * 1024;

template <typename Value>
bool invalidJson(const Result<Value>& result)
{
    const auto* error = std::get_if<ContractError>(&result);
    return error != nullptr && error->code == ErrorCode::invalidJson;
}
}

TEST_CASE("Contract JSON accepts the byte limit and rejects one extra byte", "[contracts][hardening]")
{
    auto patch = std::get<std::string>(encodePatch({}));
    patch.resize(maximumBytes, ' ');
    REQUIRE(std::holds_alternative<InstrumentPatch>(decodePatch(patch)));
    patch.push_back(' ');
    CHECK(invalidJson(decodePatch(patch)));

    auto command = std::get<std::string>(encodeCommand({"session-a", 0, {}}));
    command.resize(maximumBytes, ' ');
    REQUIRE(std::holds_alternative<ProjectCommand>(decodeCommand(command)));
    command.push_back(' ');
    CHECK(invalidJson(decodeCommand(command)));
}

TEST_CASE("Contract JSON rejects a leading UTF-8 BOM", "[contracts][hardening]")
{
    const auto patch = std::get<std::string>(encodePatch({}));
    const auto command = std::get<std::string>(encodeCommand({"session-a", 0, {}}));
    CHECK(invalidJson(decodePatch(std::string("\xef\xbb\xbf") + patch)));
    CHECK(invalidJson(decodeCommand(std::string("\xef\xbb\xbf") + command)));
}

TEST_CASE("Contract JSON rejects excess nesting during parsing", "[contracts][hardening]")
{
    auto patch = nlohmann::json::parse(std::get<std::string>(encodePatch({})));
    auto command = nlohmann::json::parse(std::get<std::string>(encodeCommand({"session-a", 0, {}})));
    REQUIRE(std::holds_alternative<ProjectCommand>(decodeCommand(command.dump())));
    command["patch"]["gain_db"] = nlohmann::json::object();
    CHECK(invalidJson(decodeCommand(command.dump())));
    command["patch"]["gain_db"] = nlohmann::json::array();
    CHECK(invalidJson(decodeCommand(command.dump())));
    patch["gain_db"] = nlohmann::json::array({nlohmann::json::array({0})});
    CHECK(invalidJson(decodePatch(patch.dump())));
    const auto deep = std::string(256, '[') + "0" + std::string(256, ']');
    CHECK(invalidJson(decodePatch(deep)));
    CHECK(invalidJson(decodeCommand(deep)));
}
