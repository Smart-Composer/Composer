#include "RecordingDuration.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

namespace
{
using namespace composer::app;

struct Fixture
{
    CaptureMapping mapping{7, 10, 48000.0, -384, 256};
    std::optional<CaptureClockSnapshot> endpoint{CaptureClockSnapshot{7, 20, 20, 480640, 480384,
                                                                    48000.0, -100.0, true}};
    PendingTake take;
    Fixture() { take.captured.capacity = 16; }
    CompletionDuration finish() const { return recordingDuration(endpoint, mapping, take); }
};

void isError(const CompletionDuration& result, CompletionError wanted)
{
    REQUIRE(std::holds_alternative<CompletionError>(result));
    CHECK(std::get<CompletionError>(result) == wanted);
}

void isDuration(const CompletionDuration& result, double wanted)
{
    REQUIRE(std::holds_alternative<double>(result));
    CHECK(std::get<double>(result) == wanted);
}
}

TEST_CASE("Completion preserves endpoint silence and covers late retained events without changing them")
{
    Fixture fixture;
    isDuration(fixture.finish(), 10.0);
    fixture.take.captured.events = {{0.0, {0x90, 60, 91}}, {0.125, {0x80, 60, 17}}};
    const auto originals = fixture.take.captured.events;
    isDuration(fixture.finish(), 10.0);
    fixture.endpoint->graphEditEnd = std::numeric_limits<std::int64_t>::max();
    isDuration(fixture.finish(), 10.0);
    fixture.endpoint->streamEnd = 640;
    isDuration(fixture.finish(), 0.125);
    CHECK(fixture.take.captured.events == originals);
    fixture.take.captured.events.clear();
    isDuration(fixture.finish(), 0.0);
    fixture.endpoint->streamEnd = 641;
    fixture.take.captured.events = {{17.0 / 48000.0, {0x9f, 67, 93}},
                                    {109.0 / 48000.0, {0x8f, 67, 41}}};
    isDuration(fixture.finish(), 109.0 / 48000.0);
    CHECK(fixture.take.captured.events.back().timeSeconds == 109.0 / 48000.0);
    fixture.endpoint->streamEnd = 639;
    isError(fixture.finish(), CompletionError::invalidEndpoint);
}

TEST_CASE("Interrupted or lossy capture cannot become a completed duration")
{
    Fixture fixture;
    fixture.take.interruptionFaults = static_cast<unsigned>(CaptureInterruption::generationChanged);
    isError(fixture.finish(), CompletionError::incompleteCapture);
    fixture.endpoint.reset();
    isError(fixture.finish(), CompletionError::incompleteCapture);
    fixture = Fixture{};
    fixture.take.captured.invalidTimestamps = 1;
    isError(fixture.finish(), CompletionError::incompleteCapture);
    fixture = Fixture{};
    fixture.take.captured.invalidMessages = 1;
    isError(fixture.finish(), CompletionError::incompleteCapture);
    fixture = Fixture{};
    fixture.take.captured.overflowEvents = 1;
    isError(fixture.finish(), CompletionError::incompleteCapture);
    fixture = Fixture{};
    fixture.take.captured.ignoredSystemMessages = 17;
    isDuration(fixture.finish(), 10.0);
}

TEST_CASE("Completion requires a coherent matching epoch with finite valid timing")
{
    Fixture fixture;
    fixture.endpoint.reset();
    isError(fixture.finish(), CompletionError::unavailableSnapshot);
    fixture = Fixture{};
    fixture.endpoint->generation++;
    isError(fixture.finish(), CompletionError::incompatibleSnapshot);
    fixture = Fixture{};
    fixture.endpoint->rate = 44100;
    isError(fixture.finish(), CompletionError::incompatibleSnapshot);
    fixture = Fixture{};
    fixture.endpoint->serial = fixture.mapping.proofSerial - 1;
    isError(fixture.finish(), CompletionError::incompatibleSnapshot);
    fixture = Fixture{};
    fixture.endpoint->valid = false;
    isError(fixture.finish(), CompletionError::incompatibleSnapshot);
    for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()})
    {
        fixture = Fixture{};
        fixture.endpoint->correction = invalid;
        isError(fixture.finish(), CompletionError::incompatibleSnapshot);
        fixture = Fixture{};
        fixture.endpoint->rate = invalid;
        isError(fixture.finish(), CompletionError::incompatibleSnapshot);
        fixture = Fixture{};
        fixture.mapping.sampleRate = invalid;
        isError(fixture.finish(), CompletionError::invalidMapping);
    }
    for (const auto invalidRate : {0.0, -1.0})
    {
        fixture = Fixture{};
        fixture.mapping.sampleRate = invalidRate;
        isError(fixture.finish(), CompletionError::invalidMapping);
    }
    fixture = Fixture{};
    fixture.mapping.generation = 0;
    isError(fixture.finish(), CompletionError::invalidMapping);
    fixture.mapping.generation = std::numeric_limits<std::uint64_t>::max();
    isError(fixture.finish(), CompletionError::invalidMapping);
    fixture = Fixture{};
    fixture.mapping.proofSerial = 0;
    isError(fixture.finish(), CompletionError::invalidMapping);
    fixture = Fixture{};
    fixture.mapping.takeStartEditSample = -1;
    isError(fixture.finish(), CompletionError::invalidMapping);
    fixture = Fixture{};
    fixture.endpoint->streamEnd = -1;
    isError(fixture.finish(), CompletionError::invalidEndpoint);
}

TEST_CASE("Completion rejects invalid events and inclusive-duration overflow instead of clamping")
{
    Fixture fixture;
    for (const auto invalidTime : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity()})
    {
        fixture.take.captured.events = {{invalidTime, {0x90, 60, 90}}};
        isError(fixture.finish(), CompletionError::invalidCapture);
    }
    fixture.take.captured.events = {{1.0, {0x90, 60, 90}}, {0.5, {0x80, 60, 90}}};
    isError(fixture.finish(), CompletionError::invalidCapture);
    fixture.take.captured.events.clear();
    fixture.take.captured.capacity = 0;
    isError(fixture.finish(), CompletionError::invalidCapture);
    fixture.take.captured.capacity = composer::project::maxProjectEvents + 1;
    isError(fixture.finish(), CompletionError::invalidCapture);
    fixture.take.captured.capacity = 1;
    fixture.take.captured.events = {{0.0, {0x90, 60, 90}}, {0.5, {0x80, 60, 90}}};
    isError(fixture.finish(), CompletionError::invalidCapture);
    fixture = Fixture{};
    fixture.take.captured.events = {{composer::project::maxProjectDurationSeconds, {0x90, 60, 90}}};
    isDuration(fixture.finish(), composer::project::maxProjectDurationSeconds);
    fixture.take.captured.events[0].timeSeconds = std::nextafter(composer::project::maxProjectDurationSeconds,
                                                                std::numeric_limits<double>::infinity());
    isError(fixture.finish(), CompletionError::durationExceeded);
    fixture = Fixture{};
    fixture.endpoint->streamEnd = 640 + 86400LL * 48000;
    isDuration(fixture.finish(), composer::project::maxProjectDurationSeconds);
    fixture.endpoint->streamEnd++;
    isError(fixture.finish(), CompletionError::durationExceeded);
    fixture = Fixture{};
    fixture.mapping.sampleRate = std::numeric_limits<double>::denorm_min();
    fixture.endpoint->rate = fixture.mapping.sampleRate;
    isError(fixture.finish(), CompletionError::invalidEndpoint);
}

TEST_CASE("Completion checks integer limits before translating the recorded endpoint")
{
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    Fixture fixture;
    fixture.mapping.streamToEditOffset = low;
    fixture.mapping.takeStartEditSample = 1;
    isError(fixture.finish(), CompletionError::invalidMapping);
    fixture.mapping.takeStartEditSample = 0;
    fixture.endpoint->streamEnd = high;
    isError(fixture.finish(), CompletionError::invalidEndpoint);

    fixture.mapping.streamToEditOffset = high;
    fixture.endpoint->streamEnd = 1;
    isError(fixture.finish(), CompletionError::invalidEndpoint);
    fixture.mapping.takeStartEditSample = high;
    fixture.mapping.sampleRate = 48000.0;
    fixture.endpoint->rate = 48000.0;
    fixture.endpoint->streamEnd = 48000;
    isDuration(fixture.finish(), 1.0);
    fixture.mapping.takeStartEditSample = high - 1;
    isDuration(fixture.finish(), 48001.0 / 48000.0);

    fixture.mapping.sampleRate = 2.0e14;
    fixture.endpoint->rate = 2.0e14;
    fixture.endpoint->streamEnd = high;
    isError(fixture.finish(), CompletionError::invalidEndpoint);
    fixture.mapping.takeStartEditSample = high;
    isDuration(fixture.finish(), static_cast<double>(high) / 2.0e14);
}
