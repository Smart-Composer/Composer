#include <composer/project/MidiCaptureBuffer.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
using namespace composer::project;

constexpr std::array<std::uint8_t, 3> noteOn { 0x90, 60, 100 };

std::vector<MidiEvent> sorted(std::vector<MidiEvent> events)
{
    std::sort(events.begin(), events.end(),
              [](const auto& left, const auto& right)
              { return left.timeSeconds < right.timeSeconds; });
    return events;
}
}

TEST_CASE("MIDI capture has explicit capacity and take lifecycle", "[project][capture]")
{
    REQUIRE_THROWS_AS(MidiCaptureBuffer(0), std::invalid_argument);
    REQUIRE_THROWS_AS(MidiCaptureBuffer(maxProjectEvents + 1), std::invalid_argument);
    MidiCaptureBuffer capture(2);
    REQUIRE(capture.submit(0.0, noteOn) == CaptureSubmitResult::inactive);
    REQUIRE_FALSE(capture.finish().has_value());
    REQUIRE(capture.start() == CaptureStartResult::started);
    REQUIRE(capture.start() == CaptureStartResult::alreadyActive);
    const auto empty = capture.finish();
    REQUIRE(empty.has_value());
    REQUIRE(empty->events.empty());
    REQUIRE(empty->capacity == 2);
    REQUIRE(empty->isComplete());
    REQUIRE_FALSE(capture.finish().has_value());
    REQUIRE(capture.start() == CaptureStartResult::started);
    REQUIRE(capture.submit(maxProjectDurationSeconds, noteOn) == CaptureSubmitResult::accepted);
    REQUIRE(capture.finish()->events.front().timeSeconds == maxProjectDurationSeconds);
}

TEST_CASE("MIDI capture preserves every channel message and equal-time order", "[project][capture]")
{
    const std::vector<MidiEvent> messages {
        { 0.125000001, { 0x8f, 60, 93 } },
        { 0.125000001, { 0x90, 61, 0 } },
        { 0.125000001, { 0xa1, 60, 127 } },
        { 0.125000001, { 0xb2, 64, 127 } },
        { 0.125000001, { 0xc3, 127 } },
        { 0.125000001, { 0xd4, 77 } },
        { 0.125000001, { 0xe5, 3, 64 } },
        { 0.0, { 0x90, 60, 100 } }
    };
    MidiCaptureBuffer capture(messages.size());
    REQUIRE(capture.start() == CaptureStartResult::started);
    for (const auto& event : messages)
        REQUIRE(capture.submit(event.timeSeconds, event.bytes) == CaptureSubmitResult::accepted);
    auto expected = messages;
    std::rotate(expected.begin(), expected.end() - 1, expected.end());
    const auto result = capture.finish();
    REQUIRE(result.has_value());
    REQUIRE(result->isComplete());
    REQUIRE(result->events == expected);
}

TEST_CASE("MIDI capture exposes rejected input without consuming event capacity", "[project][capture]")
{
    MidiCaptureBuffer capture(1);
    REQUIRE(capture.start() == CaptureStartResult::started);
    const std::array invalidTimes {
        -0.01, maxProjectDurationSeconds + 0.01,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()
    };
    for (const auto time : invalidTimes)
        REQUIRE(capture.submit(time, noteOn) == CaptureSubmitResult::invalidTimestamp);
    const std::vector<std::vector<std::uint8_t>> malformed {
        {}, { 60, 100 }, { 0x90 }, { 0x90, 60 }, { 0x90, 60, 100, 0 },
        { 0xc0 }, { 0xc0, 1, 2 }, { 0xd0, 128 }, { 0x90, 128, 100 },
        { 0x90, 60, 128 }, { 0x80, 60, 255 }
    };
    for (const auto& message : malformed)
        REQUIRE(capture.submit(0.0, message) == CaptureSubmitResult::invalidMessage);
    const std::array<std::uint8_t, 1> clock { 0xf8 };
    const std::array<std::uint8_t, 5> sysex { 0xf0, 1, 2, 3, 0xf7 };
    REQUIRE(capture.submit(0.0, clock) == CaptureSubmitResult::ignoredSystemMessage);
    REQUIRE(capture.submit(0.0, sysex) == CaptureSubmitResult::ignoredSystemMessage);
    REQUIRE(capture.submit(0.0, noteOn) == CaptureSubmitResult::accepted);
    const auto result = capture.finish();
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->isComplete());
    REQUIRE(result->events.size() == 1);
    REQUIRE(result->invalidTimestamps == invalidTimes.size());
    REQUIRE(result->invalidMessages == malformed.size());
    REQUIRE(result->ignoredSystemMessages == 2);
    REQUIRE(result->overflowEvents == 0);
}

TEST_CASE("System MIDI exclusion and overflow have distinct completeness", "[project][capture]")
{
    MidiCaptureBuffer capture(1);
    REQUIRE(capture.start() == CaptureStartResult::started);
    const std::array<std::uint8_t, 1> activeSensing { 0xfe };
    REQUIRE(capture.submit(0.0, activeSensing) == CaptureSubmitResult::ignoredSystemMessage);
    REQUIRE(capture.finish()->isComplete());
    REQUIRE(capture.start() == CaptureStartResult::started);
    REQUIRE(capture.submit(0.25, noteOn) == CaptureSubmitResult::accepted);
    for (int i = 0; i < 7; ++i)
        REQUIRE(capture.submit(0.5, noteOn) == CaptureSubmitResult::overflow);
    const auto result = capture.finish();
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->isComplete());
    REQUIRE(result->events == std::vector<MidiEvent> { { 0.25, { 0x90, 60, 100 } } });
    REQUIRE(result->overflowEvents == 7);
    REQUIRE(result->ignoredSystemMessages == 0);
    REQUIRE(capture.start() == CaptureStartResult::started);
    REQUIRE(capture.submit(0.75, noteOn) == CaptureSubmitResult::accepted);
    const auto next = capture.finish();
    REQUIRE(next->isComplete());
    REQUIRE(next->events.front().timeSeconds == 0.75);
    REQUIRE(next->overflowEvents == 0);
}

TEST_CASE("Concurrent MIDI producers retain all distinct event data", "[project][capture]")
{
    constexpr std::size_t producerCount = 4;
    constexpr std::size_t eventsPerProducer = 1000;
    MidiCaptureBuffer capture(producerCount * eventsPerProducer);
    REQUIRE(capture.start() == CaptureStartResult::started);
    std::barrier launch(static_cast<std::ptrdiff_t>(producerCount + 1));
    std::atomic<std::size_t> accepted { 0 };
    std::vector<std::thread> producers;
    std::vector<MidiEvent> expected;
    for (std::size_t producer = 0; producer < producerCount; ++producer)
    {
        for (std::size_t i = 0; i < eventsPerProducer; ++i)
            expected.push_back({ static_cast<double>(producer * eventsPerProducer + i) / 48000.0,
                                 { static_cast<std::uint8_t>(0x90 + producer),
                                   static_cast<std::uint8_t>(i % 128),
                                   static_cast<std::uint8_t>(i / 128) } });
        producers.emplace_back([&, producer]
        {
            launch.arrive_and_wait();
            for (std::size_t i = 0; i < eventsPerProducer; ++i)
            {
                const std::array<std::uint8_t, 3> message {
                    static_cast<std::uint8_t>(0x90 + producer),
                    static_cast<std::uint8_t>(i % 128), static_cast<std::uint8_t>(i / 128)
                };
                if (capture.submit(static_cast<double>(producer * eventsPerProducer + i) / 48000.0,
                                   message) == CaptureSubmitResult::accepted)
                    accepted.fetch_add(1);
            }
        });
    }
    launch.arrive_and_wait();
    for (auto& producer : producers)
        producer.join();
    const auto result = capture.finish();
    REQUIRE(result.has_value());
    REQUIRE(result->isComplete());
    REQUIRE(accepted.load() == expected.size());
    REQUIRE(result->events == sorted(expected));
}

TEST_CASE("MIDI producers can straddle repeated finish and start boundaries", "[project][capture]")
{
    constexpr std::size_t producerCount = 4;
    constexpr std::size_t epochs = 100;
    constexpr std::size_t eventsPerEpoch = 32;
    MidiCaptureBuffer capture(producerCount * epochs * eventsPerEpoch);
    std::barrier phase(static_cast<std::ptrdiff_t>(producerCount + 1));
    std::array<std::vector<MidiEvent>, producerCount> accepted;
    std::vector<std::thread> producers;
    for (std::size_t producer = 0; producer < producerCount; ++producer)
    {
        producers.emplace_back([&, producer]
        {
            for (std::size_t epoch = 0; epoch < epochs; ++epoch)
            {
                phase.arrive_and_wait();
                for (std::size_t event = 0; event < eventsPerEpoch; ++event)
                {
                    const auto id = producer * epochs * eventsPerEpoch + epoch * eventsPerEpoch + event;
                    const auto time = static_cast<double>(id) / 48000.0;
                    const std::array<std::uint8_t, 3> message {
                        static_cast<std::uint8_t>(0x90 + producer),
                        static_cast<std::uint8_t>(id % 128),
                        static_cast<std::uint8_t>((id / 128) % 128)
                    };
                    if (capture.submit(time, message) == CaptureSubmitResult::accepted)
                        accepted[producer].push_back({ time, { message.begin(), message.end() } });
                    std::this_thread::yield();
                }
                phase.arrive_and_wait();
            }
        });
    }
    std::vector<MidiEvent> captured;
    bool complete = true;
    for (std::size_t epoch = 0; epoch < epochs; ++epoch)
    {
        const auto firstStart = capture.start();
        complete = complete && firstStart == CaptureStartResult::started;
        phase.arrive_and_wait();
        for (int transition = 0; transition < 4; ++transition)
        {
            std::this_thread::yield();
            auto result = capture.finish();
            complete = complete && result.has_value() && result->isComplete();
            if (result)
                captured.insert(captured.end(), result->events.begin(), result->events.end());
            const auto nextStart = capture.start();
            complete = complete && nextStart == CaptureStartResult::started;
        }
        phase.arrive_and_wait();
        auto result = capture.finish();
        complete = complete && result.has_value() && result->isComplete();
        if (result)
            captured.insert(captured.end(), result->events.begin(), result->events.end());
    }
    for (auto& producer : producers)
        producer.join();
    std::vector<MidiEvent> expected;
    for (const auto& part : accepted)
        expected.insert(expected.end(), part.begin(), part.end());
    REQUIRE(complete);
    REQUIRE_FALSE(expected.empty());
    REQUIRE(sorted(captured) == sorted(expected));
}
