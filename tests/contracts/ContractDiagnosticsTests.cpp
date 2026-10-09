#include <composer/contracts/ProjectCommand.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace composer::contracts;
using Json = nlohmann::json;

namespace
{
template <class T>
void requireError(const Result<T>& result, ErrorCode code, std::string_view field)
{
    REQUIRE(std::holds_alternative<ContractError>(result));
    const auto& error = std::get<ContractError>(result);
    CHECK(error.code == code);
    CHECK(error.field == field);
    CHECK_FALSE(error.message.empty());
}

Json defaultPatchJson()
{
    const auto encoded = encodePatch(InstrumentPatch{});
    REQUIRE(std::holds_alternative<std::string>(encoded));
    return Json::parse(std::get<std::string>(encoded));
}

Json defaultCommandJson()
{
    return Json{{"schema_version", 1}, {"type", "replace_instrument_patch"},
                {"project_instance_id", "session-a"}, {"expected_revision", 0},
                {"patch", defaultPatchJson()}};
}

std::string withVersionToken(Json object, std::string_view token)
{
    object.erase("schema_version");
    auto text = object.dump();
    text.pop_back();
    text += ",\"schema_version\":";
    text += token;
    text += '}';
    return text;
}

std::string withPatchSource(std::string_view patchSource)
{
    auto object = defaultCommandJson();
    object.erase("patch");
    auto text = object.dump();
    text.pop_back();
    text += ",\"patch\":";
    text += patchSource;
    text += '}';
    return text;
}

std::vector<std::string> encounteredKeys(const nlohmann::ordered_json& object)
{
    REQUIRE(object.is_object());
    std::vector<std::string> keys;
    for (const auto& item : object.items())
        keys.push_back(item.key());
    return keys;
}
}

TEST_CASE("Contract diagnostics identify object structure and nested fields", "[contracts]")
{
    for (const auto source : {"[]", "null", "42"})
    {
        INFO(source);
        requireError(decodePatch(source), ErrorCode::invalidPatch, "");
        requireError(decodeCommand(source), ErrorCode::invalidCommand, "");
        requireError(decodeCommand(withPatchSource(source)), ErrorCode::invalidCommand, "patch");
    }
    requireError(decodePatch("{"), ErrorCode::invalidJson, "");
    requireError(decodeCommand("{"), ErrorCode::invalidJson, "");

    auto patch = defaultPatchJson();
    patch.erase("schema_version");
    requireError(decodePatch(patch.dump()), ErrorCode::invalidPatch, "schema_version");
    requireError(decodeCommand(withPatchSource(patch.dump())), ErrorCode::invalidCommand,
                 "patch.schema_version");
    for (const auto& descriptor : parameterDescriptors)
    {
        INFO(descriptor.id);
        patch = defaultPatchJson();
        patch.erase(std::string(descriptor.id));
        requireError(decodePatch(patch.dump()), ErrorCode::invalidPatch, descriptor.id);
        requireError(decodeCommand(withPatchSource(patch.dump())), ErrorCode::invalidCommand,
                     "patch." + std::string(descriptor.id));
    }

    patch = defaultPatchJson();
    patch["extra"] = 1;
    requireError(decodePatch(patch.dump()), ErrorCode::invalidPatch, "");
    requireError(decodeCommand(withPatchSource(patch.dump())), ErrorCode::invalidCommand, "patch");
    patch.erase("gain_db");
    requireError(decodePatch(patch.dump()), ErrorCode::invalidPatch, "gain_db");
    patch.erase("extra");
    patch["waveform"] = false;
    requireError(decodePatch(patch.dump()), ErrorCode::invalidPatch, "gain_db");

    patch = defaultPatchJson();
    patch["gain_db"] = 2;
    requireError(decodePatch(patch.dump()), ErrorCode::invalidPatch, "gain_db");
    requireError(decodeCommand(withPatchSource(patch.dump())), ErrorCode::invalidCommand, "patch.gain_db");

    for (const auto* field : {"schema_version", "type", "project_instance_id", "expected_revision", "patch"})
    {
        INFO(field);
        auto command = defaultCommandJson();
        command.erase(field);
        requireError(decodeCommand(command.dump()), ErrorCode::invalidCommand, field);
    }
    auto command = defaultCommandJson();
    command["extra"] = 1;
    requireError(decodeCommand(command.dump()), ErrorCode::invalidCommand, "");
    command.erase("patch");
    requireError(decodeCommand(command.dump()), ErrorCode::invalidCommand, "patch");
    for (const auto* field : {"type", "project_instance_id", "expected_revision"})
    {
        INFO(field);
        command = defaultCommandJson();
        command[field] = false;
        requireError(decodeCommand(command.dump()), ErrorCode::invalidCommand, field);
    }
}

TEST_CASE("Version diagnostics distinguish unsupported integer tokens from invalid tokens", "[contracts]")
{
    struct VersionCase { std::string_view token; bool unsupported; };
    const std::array<VersionCase, 13> cases{{
        {"0", true}, {"2", true}, {"4294967295", true}, {"4294967296", true},
        {"18446744073709551615", true}, {"18446744073709551616", false},
        {"-1", false}, {"1.0", false}, {"1e0", false}, {"1.0000000000000001", false},
        {"true", false}, {"null", false}, {"\"1\"", false}
    }};
    for (const auto& test : cases)
    {
        INFO(test.token);
        const auto patchCode = test.unsupported ? ErrorCode::unsupportedVersion : ErrorCode::invalidPatch;
        const auto commandCode = test.unsupported ? ErrorCode::unsupportedVersion : ErrorCode::invalidCommand;
        const auto patchSource = withVersionToken(defaultPatchJson(), test.token);
        requireError(decodePatch(patchSource), patchCode, "schema_version");
        requireError(decodeCommand(withVersionToken(defaultCommandJson(), test.token)),
                     commandCode, "schema_version");
        requireError(decodeCommand(withPatchSource(patchSource)), commandCode, "patch.schema_version");
    }
    auto patch = defaultPatchJson();
    patch.erase("gain_db");
    requireError(decodePatch(withVersionToken(patch, "2")), ErrorCode::unsupportedVersion, "schema_version");
    auto command = defaultCommandJson();
    command.erase("patch");
    requireError(decodeCommand(withVersionToken(command, "2")), ErrorCode::unsupportedVersion, "schema_version");
}

TEST_CASE("Command rejection precedence preserves populated undo and redo", "[contracts]")
{
    const auto a = InstrumentPatch{};
    auto b = a;
    b.gainDb = -18;
    auto c = a;
    c.gainDb = -24;
    auto created = ProjectPatchSession::create("session-a", a, maximumProjectRevision - 3);
    REQUIRE(std::holds_alternative<ProjectPatchSession>(created));
    auto session = std::move(std::get<ProjectPatchSession>(created));
    const auto requireApplied = [](const Result<EditOutcome>& result)
    {
        REQUIRE(std::holds_alternative<EditOutcome>(result));
        REQUIRE(std::get<EditOutcome>(result) == EditOutcome::applied);
    };
    requireApplied(session.apply({"session-a", maximumProjectRevision - 3, b}));
    requireApplied(session.apply({"session-a", maximumProjectRevision - 2, c}));
    requireApplied(session.undo());
    const auto before = session.snapshot();
    REQUIRE(before.revision == maximumProjectRevision);
    REQUIRE(before.patch == b);
    const std::vector<InstrumentPatch> undoBefore(session.undoHistory().begin(), session.undoHistory().end());
    const std::vector<InstrumentPatch> redoBefore(session.redoHistory().begin(), session.redoHistory().end());
    REQUIRE(undoBefore.size() == 1);
    REQUIRE(redoBefore.size() == 1);
    REQUIRE(undoBefore.front() == a);
    REQUIRE(redoBefore.front() == c);
    const auto unchanged = [&]
    {
        CHECK(session.snapshot().projectInstanceId == before.projectInstanceId);
        CHECK(session.snapshot().revision == before.revision);
        CHECK(session.snapshot().patch == before.patch);
        const std::vector<InstrumentPatch> undoNow(session.undoHistory().begin(), session.undoHistory().end());
        const std::vector<InstrumentPatch> redoNow(session.redoHistory().begin(), session.redoHistory().end());
        CHECK(undoNow == undoBefore);
        CHECK(redoNow == redoBefore);
    };

    auto invalid = c;
    invalid.cutoffHz = 0;
    requireError(session.apply({"session-b", maximumProjectRevision - 1, invalid}),
                 ErrorCode::invalidCommand, "patch.cutoff_hz");
    unchanged();
    requireError(session.apply({"session-b", maximumProjectRevision - 1, b}),
                 ErrorCode::wrongProject, "project_instance_id");
    unchanged();
    requireError(session.apply({"session-a", maximumProjectRevision - 1, b}),
                 ErrorCode::staleRevision, "expected_revision");
    unchanged();
    const auto equal = session.apply({"session-a", maximumProjectRevision, b});
    REQUIRE(std::holds_alternative<EditOutcome>(equal));
    CHECK(std::get<EditOutcome>(equal) == EditOutcome::noChange);
    unchanged();
    requireError(session.apply({"session-a", maximumProjectRevision, c}),
                 ErrorCode::revisionExhausted, "expected_revision");
    unchanged();
}

TEST_CASE("Contract serialization emits lexicographic keys without pinning number spellings", "[contracts]")
{
    const std::vector<std::string> patchKeys{
        "attack_seconds", "cutoff_hz", "decay_seconds", "gain_db", "release_seconds",
        "resonance_q", "schema_version", "sustain_level", "waveform"
    };
    const std::vector<std::string> commandKeys{
        "expected_revision", "patch", "project_instance_id", "schema_version", "type"
    };
    const auto patch = encodePatch(InstrumentPatch{});
    REQUIRE(std::holds_alternative<std::string>(patch));
    const auto orderedPatch = nlohmann::ordered_json::parse(std::get<std::string>(patch));
    CHECK(encounteredKeys(orderedPatch) == patchKeys);
    const auto command = encodeCommand({"session-a", 0, {}});
    REQUIRE(std::holds_alternative<std::string>(command));
    const auto orderedCommand = nlohmann::ordered_json::parse(std::get<std::string>(command));
    CHECK(encounteredKeys(orderedCommand) == commandKeys);
    CHECK(encounteredKeys(orderedCommand.at("patch")) == patchKeys);
}
