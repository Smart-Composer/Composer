#include <composer/project/ProjectDocument.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

using namespace composer::project;
using Json = nlohmann::json;

namespace
{
ProjectDocument performance()
{
    ProjectDocument document;
    document.patch = {composer::contracts::Waveform::saw, -9.75, 0.031, 0.22, 0.63, 0.41, 4321.125, 1.125};
    document.durationSeconds = 60.0;
    document.events = {{0.0, {0xc0, 7}}, {0.0, {0xb0, 7, 100}}, {0.0, {0xe0, 0, 64}},
                       {0.0, {0xd0, 45}}, {0.0, {0xa0, 60, 55}}};
    for (int index = 0; index < 120; ++index)
    {
        const double time = static_cast<double>(index) * 0.5 + 0.00123456789;
        const auto pitch = static_cast<std::uint8_t>(36 + index % 48);
        const auto velocity = static_cast<std::uint8_t>(1 + index % 127);
        document.events.push_back({time, {0x90, pitch, velocity}});
        document.events.push_back({time + 0.375123456789, {0x80, pitch, 64}});
    }
    document.events.push_back({60.0, {0xb0, 123, 0}});
    return document;
}

std::string encoded(const ProjectDocument& document)
{
    const auto result = encodeProject(document);
    REQUIRE(std::holds_alternative<std::string>(result));
    return std::get<std::string>(result);
}

Json validJson()
{
    return Json::parse(encoded(performance()));
}

ProjectError rejected(std::string_view source)
{
    const auto result = decodeProject(source);
    REQUIRE(std::holds_alternative<ProjectError>(result));
    return std::get<ProjectError>(result);
}

void replaceOnce(std::string& source, const std::string& before, const std::string& after)
{
    const auto position = source.find(before);
    REQUIRE(position != std::string::npos);
    source.replace(position, before.size(), after);
}
}

TEST_CASE("A one-minute performance roundtrips all notes, controls and patch values", "[project]")
{
    const auto original = performance();
    REQUIRE_FALSE(validateProject(original));
    const auto source = encoded(original);
    const auto result = decodeProject(source);
    REQUIRE(std::holds_alternative<ProjectDocument>(result));
    CHECK(std::get<ProjectDocument>(result) == original);
    CHECK(encoded(std::get<ProjectDocument>(result)) == source);
    const auto json = Json::parse(source);
    CHECK(json.size() == 5);
    CHECK(json.at("schema_version") == projectSchemaVersion);
    CHECK(json.at("tempo_bpm") == projectTempoBpm);
    CHECK(json.at("events").size() == 246);
    CHECK(json.at("patch").at("waveform") == "saw");
    CHECK(json.at("patch").at("gain_db") == -9.75);
    CHECK(original == performance());
}

TEST_CASE("Simultaneous MIDI messages preserve raw bytes and array order", "[project]")
{
    ProjectDocument document;
    document.durationSeconds = 1.0;
    document.events = {{0.5, {0x91, 60, 100}}, {0.5, {0x91, 60, 0}},
                       {0.5, {0x81, 60, 45}}, {0.5, {0xb1, 64, 127}},
                       {0.5, {0xe1, 1, 126}}, {0.5, {0xc1, 11}}, {0.5, {0xd1, 12}}};
    const auto result = decodeProject(encoded(document));
    REQUIRE(std::holds_alternative<ProjectDocument>(result));
    CHECK(std::get<ProjectDocument>(result).events == document.events);
}

TEST_CASE("Every MIDI channel-message status uses its specified byte length", "[project]")
{
    ProjectDocument document;
    for (int status = 0x80; status <= 0xef; ++status)
    {
        MidiEvent event{0.0, {static_cast<std::uint8_t>(status), 127}};
        if ((status & 0xf0) != 0xc0 && (status & 0xf0) != 0xd0)
            event.bytes.push_back(0);
        document.events.push_back(std::move(event));
    }
    REQUIRE_FALSE(validateProject(document));
    const auto result = decodeProject(encoded(document));
    REQUIRE(std::holds_alternative<ProjectDocument>(result));
    CHECK(std::get<ProjectDocument>(result) == document);
}

TEST_CASE("Empty performances and exact duration boundaries are valid", "[project]")
{
    for (const auto duration : {0.0, maxProjectDurationSeconds})
    {
        ProjectDocument document;
        document.durationSeconds = duration;
        REQUIRE_FALSE(validateProject(document));
        document.events = {{0.0, {0xc0, 0}}, {duration, {0xc0, 127}}};
        const auto result = decodeProject(encoded(document));
        REQUIRE(std::holds_alternative<ProjectDocument>(result));
        CHECK(std::get<ProjectDocument>(result) == document);
    }
}

TEST_CASE("Project and nested patch fields are complete and closed", "[project]")
{
    const auto original = validJson();
    for (const auto* name : {"schema_version", "tempo_bpm", "patch", "duration_seconds", "events"})
    {
        INFO(name);
        auto json = original;
        json.erase(name);
        CHECK(std::holds_alternative<ProjectError>(decodeProject(json.dump())));
    }
    for (const auto* name : {"time_seconds", "bytes"})
    {
        INFO(name);
        auto json = original;
        json["events"][0].erase(name);
        CHECK(std::holds_alternative<ProjectError>(decodeProject(json.dump())));
    }
    for (const auto* field : {"project_instance", "revision", "filename", "plugin", "assets", "xml"})
    {
        INFO(field);
        auto json = original;
        json[field] = "unrecognized";
        CHECK(rejected(json.dump()).code == ProjectErrorCode::invalidDocument);
    }
    auto json = original;
    json["events"][0]["channel"] = 1;
    CHECK(rejected(json.dump()).code == ProjectErrorCode::invalidDocument);
    json = original;
    json["patch"]["extra"] = 0;
    CHECK(rejected(json.dump()).code == ProjectErrorCode::invalidPatch);
    json = original;
    json["patch"].erase("release_seconds");
    CHECK(rejected(json.dump()).code == ProjectErrorCode::invalidPatch);
}

TEST_CASE("JSON duplicates, trailing data, NUL and malformed numbers are rejected", "[project]")
{
    const auto original = encoded(performance());
    for (const auto& duplicate : {std::string("\"schema_version\":1,"), std::string("\"\\u0073chema_version\":1,")})
    {
        auto source = original;
        source.insert(1, duplicate);
        CHECK(rejected(source).code == ProjectErrorCode::invalidJson);
    }
    auto source = original;
    replaceOnce(source, "\"patch\":{", "\"patch\":{\"gain_db\":-12,");
    CHECK(rejected(source).code == ProjectErrorCode::invalidJson);
    source = original;
    replaceOnce(source, "\"events\":[{", "\"events\":[{\"time_seconds\":0,");
    CHECK(rejected(source).code == ProjectErrorCode::invalidJson);
    for (const auto& suffix : {std::string("{}"), std::string(" trailing"), std::string(1, '\0'), std::string("\0{}", 3)})
        CHECK(rejected(original + suffix).code == ProjectErrorCode::invalidJson);
    source = original;
    source.insert(source.size() / 2, 1, '\0');
    CHECK(rejected(source).code == ProjectErrorCode::invalidJson);
    for (const auto* number : {"NaN", "Infinity", "-Infinity", "1e999", "01", "+1"})
    {
        INFO(number);
        source = original;
        replaceOnce(source, "\"duration_seconds\":60.0", std::string("\"duration_seconds\":") + number);
        CHECK(rejected(source).code == ProjectErrorCode::invalidJson);
    }
    for (const auto* malformed : {"", " ", "{", "[]", "null", "true", "{\"schema_version\":1,}"})
        CHECK(std::holds_alternative<ProjectError>(decodeProject(malformed)));
    CHECK(std::holds_alternative<ProjectDocument>(decodeProject(" \n\t" + original + "\r\n ")));
}

TEST_CASE("Integer schema and MIDI byte tokens remain strict through nested decoding", "[project]")
{
    const auto original = validJson();
    const std::array<Json, 7> invalidVersions {1.0, 1.5, -1, true, nullptr, "1", Json::array()};
    for (const auto& version : invalidVersions)
    {
        INFO(version.dump());
        auto json = original;
        json["schema_version"] = version;
        CHECK(rejected(json.dump()).code == ProjectErrorCode::invalidDocument);
        json = original;
        json["patch"]["schema_version"] = version;
        CHECK(rejected(json.dump()).code == ProjectErrorCode::invalidPatch);
    }
    for (const auto version : {0, 2})
    {
        auto json = original;
        json["schema_version"] = version;
        CHECK(rejected(json.dump()).code == ProjectErrorCode::unsupportedVersion);
        json = original;
        json["patch"]["schema_version"] = version;
        CHECK(rejected(json.dump()).code == ProjectErrorCode::unsupportedVersion);
    }
    for (const auto version : std::array<std::uint64_t, 2>{4294967296ULL, std::numeric_limits<std::uint64_t>::max()})
    {
        auto json = original;
        json["schema_version"] = version;
        CHECK(rejected(json.dump()).code == ProjectErrorCode::unsupportedVersion);
        json = original;
        json["patch"]["schema_version"] = version;
        CHECK(rejected(json.dump()).field == "patch.schema_version");
    }
    auto source = encoded(performance());
    replaceOnce(source, "\"schema_version\":1", "\"schema_version\":1e0");
    CHECK(std::holds_alternative<ProjectError>(decodeProject(source)));
    const std::array<Json, 8> invalidBytes {127.0, 1.5, -1, 256, true, nullptr, "127", Json::array()};
    for (const auto& byte : invalidBytes)
    {
        INFO(byte.dump());
        auto json = original;
        json["events"][0]["bytes"][1] = byte;
        CHECK(rejected(json.dump()).code == ProjectErrorCode::invalidDocument);
    }
}

TEST_CASE("Tempo, time and container types cannot be silently converted", "[project]")
{
    const std::array<Json, 5> nonNumbers {true, nullptr, "0", Json::array(), Json::object()};
    const auto original = validJson();
    for (const auto& value : nonNumbers)
    {
        auto json = original;
        json["duration_seconds"] = value;
        CHECK(std::holds_alternative<ProjectError>(decodeProject(json.dump())));
        json = original;
        json["events"][0]["time_seconds"] = value;
        CHECK(std::holds_alternative<ProjectError>(decodeProject(json.dump())));
        json = original;
        json["tempo_bpm"] = value;
        CHECK(std::holds_alternative<ProjectError>(decodeProject(json.dump())));
    }
    auto json = original;
    json["tempo_bpm"] = 119.999;
    CHECK(rejected(json.dump()).field == "tempo_bpm");
    json = original;
    json["events"] = Json::object();
    CHECK(rejected(json.dump()).field == "events");
    json = original;
    json["events"][0] = Json::array();
    CHECK(rejected(json.dump()).field == "events[0]");
    json = original;
    json["events"][0]["bytes"] = "C007";
    CHECK(rejected(json.dump()).field == "events[0].bytes");
}

TEST_CASE("Invalid MIDI statuses, data bytes and lengths fail without normalization", "[project]")
{
    const std::vector<std::vector<std::uint8_t>> invalidMessages {
        {}, {0x90}, {0x90, 60}, {0x90, 60, 100, 0}, {0xc0, 1, 2}, {0xd0, 1, 2},
        {0x00, 0, 0}, {0x7f, 0, 0}, {0xf0, 0, 0}, {0xf8, 0}, {0xff, 0},
        {0x90, 128, 0}, {0x90, 0, 128}, {0xc0, 255}
    };
    for (const auto& bytes : invalidMessages)
    {
        ProjectDocument document;
        document.events = {{0.0, bytes}};
        const auto original = document;
        REQUIRE(validateProject(document));
        CHECK(std::holds_alternative<ProjectError>(encodeProject(document)));
        auto json = validJson();
        json["events"][0]["bytes"] = bytes;
        CHECK(std::holds_alternative<ProjectError>(decodeProject(json.dump())));
        CHECK(document == original);
    }
}

TEST_CASE("Unsorted, out-of-range and nonfinite times fail before encoding", "[project]")
{
    for (const auto duration : {-0.001, maxProjectDurationSeconds + 0.001,
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()})
    {
        auto document = performance();
        document.durationSeconds = duration;
        REQUIRE(validateProject(document));
        CHECK(validateProject(document)->field == "duration_seconds");
        CHECK(std::holds_alternative<ProjectError>(encodeProject(document)));
    }
    for (const auto time : {-0.001, 60.001, std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        auto document = performance();
        document.events[0].timeSeconds = time;
        REQUIRE(validateProject(document));
        CHECK(validateProject(document)->field == "events[0].time_seconds");
        CHECK(std::holds_alternative<ProjectError>(encodeProject(document)));
    }
    auto document = performance();
    std::swap(document.events[5], document.events[6]);
    REQUIRE(validateProject(document));
    CHECK(validateProject(document)->field == "events[6].time_seconds");
    auto json = validJson();
    std::swap(json["events"][5], json["events"][6]);
    CHECK(rejected(json.dump()).field == "events[6].time_seconds");
    for (const auto time : {-0.001, 60.001})
    {
        json = validJson();
        json["events"][0]["time_seconds"] = time;
        CHECK(rejected(json.dump()).field == "events[0].time_seconds");
    }
    for (const auto duration : {-0.001, maxProjectDurationSeconds + 0.001})
    {
        json = validJson();
        json["duration_seconds"] = duration;
        CHECK(rejected(json.dump()).field == "duration_seconds");
    }
}

TEST_CASE("Patch validation is shared with instrument contracts", "[project]")
{
    auto document = performance();
    document.patch.cutoffHz = 0;
    REQUIRE(validateProject(document));
    CHECK(validateProject(document)->code == ProjectErrorCode::invalidPatch);
    CHECK(validateProject(document)->field == "patch.cutoff_hz");
    CHECK(std::holds_alternative<ProjectError>(encodeProject(document)));
    auto json = validJson();
    json["patch"]["cutoff_hz"] = 0;
    CHECK(rejected(json.dump()).field == "patch.cutoff_hz");
    document.patch.cutoffHz = std::numeric_limits<double>::quiet_NaN();
    CHECK(std::holds_alternative<ProjectError>(encodeProject(document)));
}

TEST_CASE("Project resource limits include exact boundaries", "[project]")
{
    SECTION("The event limit is inclusive")
    {
        ProjectDocument document;
        document.events.assign(maxProjectEvents, MidiEvent{0.0, {0xc0, 0}});
        REQUIRE_FALSE(validateProject(document));
        document.events.push_back({0.0, {0xc0, 0}});
        REQUIRE(validateProject(document));
        CHECK(validateProject(document)->code == ProjectErrorCode::resourceLimit);
        CHECK(std::holds_alternative<ProjectError>(encodeProject(document)));
    }
    SECTION("Too many decoded events are rejected before materializing events")
    {
        auto source = encoded(ProjectDocument{});
        std::string events;
        events.reserve((maxProjectEvents + 1) * 5);
        for (std::size_t index = 0; index < maxProjectEvents; ++index)
            events += "null,";
        events += "null";
        replaceOnce(source, "\"events\":[]", "\"events\":[" + events + "]");
        CHECK(rejected(source).code == ProjectErrorCode::resourceLimit);
    }
    SECTION("The byte limit counts the complete input including whitespace")
    {
        auto source = encoded(ProjectDocument{});
        source.resize(maxProjectJsonBytes, ' ');
        CHECK(std::holds_alternative<ProjectDocument>(decodeProject(source)));
        source.push_back(' ');
        CHECK(rejected(source).code == ProjectErrorCode::resourceLimit);
    }
    SECTION("Unbounded nesting is rejected")
    {
        CHECK(std::holds_alternative<ProjectError>(decodeProject("[[[[[[[[[[0]]]]]]]]]]")));
    }
}

TEST_CASE("Large event arrays decode within a bounded time", "[project]")
{
    constexpr std::size_t count = 100000;
    auto source = encoded(ProjectDocument{});
    std::string events;
    events.reserve(count * 37);
    for (std::size_t index = 0; index < count; ++index)
    {
        if (index != 0)
            events += ',';
        events += "{\"time_seconds\":0.0,\"bytes\":[192,0]}";
    }
    replaceOnce(source, "\"events\":[]", "\"events\":[" + events + "]");

    // Leave ample headroom for checked Debug builds while catching a parser
    // that rescans every preceding event whenever an object closes.
    const auto started = std::chrono::steady_clock::now();
    const auto result = decodeProject(source);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(std::holds_alternative<ProjectDocument>(result));
    const auto& document = std::get<ProjectDocument>(result);
    REQUIRE(document.events.size() == count);
    const MidiEvent expected{0.0, {0xc0, 0}};
    CHECK(std::all_of(document.events.begin(), document.events.end(),
                      [&](const auto& event) { return event == expected; }));
    INFO("Decode seconds: " << std::chrono::duration<double>(elapsed).count());
    CHECK(elapsed < std::chrono::seconds(10));
}

TEST_CASE("Project SAX parsing preserves duplicate, depth and diagnostic precedence", "[project]")
{
    struct Rejection
    {
        const char* source;
        ProjectErrorCode code;
        const char* field;
        const char* message;
    };
    const std::array cases {
        Rejection{"[]", ProjectErrorCode::invalidDocument, "schema_version", "Expected an integer schema version"},
        Rejection{"{\"schema_version\":1.0}", ProjectErrorCode::invalidDocument, "schema_version", "Expected an integer schema version"},
        Rejection{"{\"schema_version\":2}", ProjectErrorCode::unsupportedVersion, "schema_version", "Unsupported project schema version"},
        Rejection{"{\"schema_version\":1}", ProjectErrorCode::invalidDocument, "", "Expected exactly the version, tempo, patch, duration and events fields"},
        Rejection{"{\"schema_version\":2,\"extra\":{\"x\":[],\"\\u0078\":{}}}", ProjectErrorCode::invalidJson, "", "Duplicate JSON field"},
        Rejection{"[[[[[0]]]]]", ProjectErrorCode::invalidDocument, "", "JSON nesting exceeds the project structure"},
        Rejection{"[[[[[]]]]]", ProjectErrorCode::invalidDocument, "schema_version", "Expected an integer schema version"},
        Rejection{"[[[[[[]]]]]]", ProjectErrorCode::invalidDocument, "", "JSON nesting exceeds the project structure"},
        Rejection{"{\"schema_version\":2,\"extra\":[[[[0]]]]}", ProjectErrorCode::invalidDocument, "", "JSON nesting exceeds the project structure"},
        Rejection{"{\"schema_version\":2,\"extra\":[}", ProjectErrorCode::invalidJson, "", "Malformed or nonfinite JSON input"}
    };
    for (const auto& item : cases)
    {
        INFO(item.source);
        const auto error = rejected(item.source);
        CHECK(error.code == item.code);
        CHECK(error.field == item.field);
        CHECK(error.message == item.message);
    }

    const auto original = encoded(performance());
    CHECK(std::holds_alternative<ProjectDocument>(decodeProject(std::string("\xef\xbb\xbf") + original)));
    auto duplicate = original;
    replaceOnce(duplicate, "\"events\":[{", "\"events\":[{\"\\u0062ytes\":[],");
    const auto error = rejected(duplicate);
    CHECK(error.code == ProjectErrorCode::invalidJson);
    CHECK(error.field.empty());
    CHECK(error.message == "Duplicate JSON field");
}
