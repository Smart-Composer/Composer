#include <composer/contracts/ProjectCommand.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

using namespace composer::contracts;
using Json = nlohmann::json;

namespace
{
const auto sourceRoot = std::filesystem::path(COMPOSER_CONTRACT_SOURCE_DIR);

Json readJson(const std::filesystem::path& path)
{
    std::ifstream input(path);
    REQUIRE(input.good());
    return Json::parse(input);
}

Json fixture(const char* name)
{
    return readJson(sourceRoot / "tests" / "contracts" / "fixtures" / name);
}

Json changed(Json base, const Json& testCase)
{
    for (const auto& [key, value] : testCase.at("changes").items())
        base[key] = value;
    for (const auto& key : testCase.at("remove"))
        base.erase(key.get<std::string>());
    return base;
}

Json expandPatch(Json value, const Json& patches)
{
    if (value.is_string() && patches.contains(value.get<std::string>()))
        return patches.at(value.get<std::string>());
    return value;
}

Json expandCommand(Json value, const Json& patches)
{
    if (value.contains("patch"))
        value["patch"] = expandPatch(value["patch"], patches);
    return value;
}

InstrumentPatch patchValue(const Json& value, const Json& patches)
{
    auto result = decodePatch(expandPatch(value, patches).dump());
    REQUIRE(std::holds_alternative<InstrumentPatch>(result));
    return std::get<InstrumentPatch>(result);
}

std::string outcomeName(const Result<EditOutcome>& result)
{
    if (const auto* outcome = std::get_if<EditOutcome>(&result))
        return *outcome == EditOutcome::applied ? "applied" : "no_change";
    switch (std::get<ContractError>(result).code)
    {
        case ErrorCode::wrongProject: return "wrong_project";
        case ErrorCode::staleRevision: return "stale_revision";
        case ErrorCode::revisionExhausted: return "revision_exhausted";
        case ErrorCode::invalidCommand: return "invalid_command";
        default: return "unexpected_error";
    }
}

void checkHistory(std::span<const InstrumentPatch> actual, const Json& expected, const Json& patches)
{
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i)
        CHECK(actual[i] == patchValue(expected[i], patches));
}
}

TEST_CASE("Parameter descriptors match the shared schema and default patch", "[contracts]")
{
    const auto schema = readJson(sourceRoot / "docs" / "contracts" / "instrument-patch.schema.json");
    const auto patches = fixture("patches.json");
    CHECK(InstrumentPatch{} == patchValue("default", patches));
    std::set<std::string_view> identifiers;
    for (const auto& descriptor : parameterDescriptors)
    {
        INFO(descriptor.id);
        CHECK(identifiers.insert(descriptor.id).second);
        const auto& property = schema.at("properties").at(std::string(descriptor.id));
        if (descriptor.kind == ParameterKind::continuous)
        {
            REQUIRE(descriptor.member != nullptr);
            CHECK(descriptor.minimum == property.at("minimum").get<double>());
            CHECK(descriptor.maximum == property.at("maximum").get<double>());
            CHECK(descriptor.defaultValue == InstrumentPatch{}.*descriptor.member);
        }
        else
        {
            CHECK(descriptor.member == nullptr);
            CHECK(descriptor.minimum == 0);
            CHECK(descriptor.maximum == waveformDescriptors.size() - 1);
            for (std::size_t i = 0; i < waveformDescriptors.size(); ++i)
            {
                CHECK(static_cast<std::size_t>(waveformDescriptors[i].value) == i);
                CHECK(property.at("enum").at(i).get<std::string>() == waveformDescriptors[i].id);
            }
        }
    }
    CHECK(schema.at("properties").size() == parameterDescriptors.size() + 1);
    const auto reference = patchValue("soft_saw", patches);
    CHECK(reference.waveform != InstrumentPatch{}.waveform);
    for (const auto& descriptor : parameterDescriptors)
        if (descriptor.member != nullptr)
            CHECK(reference.*descriptor.member != descriptor.defaultValue);
}

TEST_CASE("Patch values use the neutral validation cases", "[contracts]")
{
    const auto patches = fixture("patches.json");
    const auto cases = fixture("patch-validation.json");
    for (const auto& testCase : cases.at("cases"))
    {
        DYNAMIC_SECTION(testCase.at("id").get<std::string>())
        {
            const auto input = changed(patches.at(testCase.at("base").get<std::string>()), testCase);
            auto result = decodePatch(input.dump());
            CHECK(std::holds_alternative<InstrumentPatch>(result) == (testCase.at("expected") == "valid"));
        }
    }
}

TEST_CASE("Patch decoding rejects ambiguous raw JSON", "[contracts]")
{
    const auto patches = fixture("patches.json");
    const auto cases = fixture("patch-parsing.json");
    for (const auto& testCase : cases.at("cases"))
    {
        DYNAMIC_SECTION(testCase.at("id").get<std::string>())
        {
            auto result = decodePatch(testCase.at("source").get<std::string>());
            const bool valid = testCase.at("expected") == "valid";
            REQUIRE(std::holds_alternative<InstrumentPatch>(result) == valid);
            if (valid)
                CHECK(std::get<InstrumentPatch>(result) == patchValue(testCase.at("patch"), patches));
        }
    }
}

TEST_CASE("Patch serialization preserves values and normalizes negative zero", "[contracts]")
{
    const auto patches = fixture("patches.json");
    for (const auto& [name, value] : patches.items())
    {
        INFO(name);
        const auto patch = patchValue(value, patches);
        auto encoded = encodePatch(patch);
        REQUIRE(std::holds_alternative<std::string>(encoded));
        auto decoded = decodePatch(std::get<std::string>(encoded));
        REQUIRE(std::holds_alternative<InstrumentPatch>(decoded));
        CHECK(std::get<InstrumentPatch>(decoded) == patch);
        CHECK(encodePatch(std::get<InstrumentPatch>(decoded)) == encoded);
    }
    auto patch = InstrumentPatch{};
    patch.attackSeconds = -0.0;
    auto encoded = encodePatch(patch);
    REQUIRE(std::holds_alternative<std::string>(encoded));
    const auto json = Json::parse(std::get<std::string>(encoded));
    CHECK_FALSE(std::signbit(json.at("attack_seconds").get<double>()));
}

TEST_CASE("In-memory patch validation rejects nonfinite values and invalid enums", "[contracts]")
{
    for (const auto& descriptor : parameterDescriptors)
    {
        if (descriptor.member == nullptr)
            continue;
        for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(),
                                   descriptor.minimum - 0.001, descriptor.maximum + 0.001})
        {
            INFO(descriptor.id);
            auto patch = InstrumentPatch{};
            patch.*descriptor.member = invalid;
            CHECK(validatePatch(patch).has_value());
            CHECK(std::holds_alternative<ContractError>(encodePatch(patch)));
        }
    }
    auto patch = InstrumentPatch{};
    patch.waveform = static_cast<Waveform>(255);
    CHECK(validatePatch(patch).has_value());
    CHECK(std::holds_alternative<ContractError>(encodePatch(patch)));
}

TEST_CASE("Commands use the neutral validation and parsing cases", "[contracts]")
{
    const auto patches = fixture("patches.json");
    const auto cases = fixture("command-validation.json");
    for (const auto& testCase : cases.at("cases"))
    {
        DYNAMIC_SECTION(testCase.at("id").get<std::string>())
        {
            const auto input = expandCommand(changed(cases.at("base"), testCase), patches);
            auto result = decodeCommand(input.dump());
            const bool valid = testCase.at("expected") == "valid";
            REQUIRE(std::holds_alternative<ProjectCommand>(result) == valid);
            if (valid)
            {
                auto encoded = encodeCommand(std::get<ProjectCommand>(result));
                REQUIRE(std::holds_alternative<std::string>(encoded));
                auto reloaded = decodeCommand(std::get<std::string>(encoded));
                REQUIRE(std::holds_alternative<ProjectCommand>(reloaded));
                const auto& original = std::get<ProjectCommand>(result);
                const auto& restored = std::get<ProjectCommand>(reloaded);
                CHECK(restored.projectInstanceId == original.projectInstanceId);
                CHECK(restored.expectedRevision == original.expectedRevision);
                CHECK(restored.patch == original.patch);
                CHECK(encodeCommand(restored) == encoded);
            }
        }
    }
    const auto rawCases = fixture("command-parsing.json");
    for (const auto& testCase : rawCases.at("cases"))
    {
        DYNAMIC_SECTION(testCase.at("id").get<std::string>())
        {
            const auto result = decodeCommand(testCase.at("source").get<std::string>());
            CHECK(std::holds_alternative<ProjectCommand>(result) == (testCase.at("expected") == "valid"));
        }
    }
}

TEST_CASE("Project patch edits preserve scope, history and monotonic revisions", "[contracts]")
{
    const auto patches = fixture("patches.json");
    const auto scenarios = fixture("command-scenarios.json");
    for (const auto& scenario : scenarios.at("scenarios"))
    {
        DYNAMIC_SECTION(scenario.at("id").get<std::string>())
        {
            const auto& initial = scenario.at("initial");
            auto created = ProjectPatchSession::create(
                initial.at("project_instance_id").get<std::string>(), patchValue(initial.at("patch"), patches),
                initial.at("revision").get<std::uint64_t>());
            REQUIRE(std::holds_alternative<ProjectPatchSession>(created));
            auto session = std::move(std::get<ProjectPatchSession>(created));
            REQUIRE(initial.at("undo").empty());
            REQUIRE(initial.at("redo").empty());
            for (const auto& step : scenario.at("steps"))
            {
                INFO(step.dump());
                Result<EditOutcome> result = EditOutcome::noChange;
                if (step.at("action") == "apply")
                {
                    auto command = decodeCommand(expandCommand(step.at("command"), patches).dump());
                    if (const auto* parsed = std::get_if<ProjectCommand>(&command))
                        result = session.apply(*parsed);
                    else
                        result = ContractError{ErrorCode::invalidCommand, {}, {}};
                }
                else if (step.at("action") == "undo")
                    result = session.undo();
                else if (step.at("action") == "redo")
                    result = session.redo();
                else
                    FAIL("Unknown fixture action");

                const auto& expected = step.at("expected");
                CHECK(outcomeName(result) == expected.at("result").get<std::string>());
                CHECK(session.snapshot().projectInstanceId == expected.at("project_instance_id").get<std::string>());
                CHECK(session.snapshot().revision == expected.at("revision").get<std::uint64_t>());
                CHECK(session.snapshot().patch == patchValue(expected.at("patch"), patches));
                checkHistory(session.undoHistory(), expected.at("undo"), patches);
                checkHistory(session.redoHistory(), expected.at("redo"), patches);
            }
        }
    }
}

TEST_CASE("Invalid typed commands cannot bypass session validation", "[contracts]")
{
    auto created = ProjectPatchSession::create("session-a");
    REQUIRE(std::holds_alternative<ProjectPatchSession>(created));
    auto session = std::move(std::get<ProjectPatchSession>(created));
    auto invalid = ProjectCommand{"session-a", 0, {}};
    invalid.patch.cutoffHz = std::numeric_limits<double>::quiet_NaN();
    CHECK(std::holds_alternative<ContractError>(session.apply(invalid)));
    CHECK(session.snapshot().patch == InstrumentPatch{});
    CHECK(session.snapshot().revision == 0);
    CHECK(session.undoHistory().empty());
    CHECK(session.redoHistory().empty());
    CHECK(std::holds_alternative<ContractError>(ProjectPatchSession::create("")));
    CHECK(std::holds_alternative<ContractError>(ProjectPatchSession::create("session-a", {}, maximumProjectRevision + 1)));
}
