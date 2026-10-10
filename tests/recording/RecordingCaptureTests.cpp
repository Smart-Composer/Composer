#include "RecordingCapture.h"
#include "AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <semaphore>
#include <thread>
#include <vector>

namespace
{
using composer::app::CaptureClockSignals;
using composer::app::CaptureInterruption;
using composer::app::CaptureMapping;
using composer::app::RecordingCapture;

struct ClockFixture
{
    CaptureClockSignals signals;
    CaptureMapping mapping{1, 10, 48000.0, 0, 0};
    ClockFixture()
    {
        signals.generation.store(mapping.generation);
        signals.serial.store(mapping.proofSerial);
        signals.correction.store(0.0);
    }
};

juce::MidiMessage note(int pitch, double time)
{
    auto message = juce::MidiMessage::noteOn(3, pitch, static_cast<juce::uint8>(91));
    message.setTimeStamp(time);
    return message;
}

bool hasFault(const RecordingCapture& capture, CaptureInterruption fault)
{
    return (capture.interruptionFaults() & static_cast<unsigned>(fault)) != 0;
}

void checkEvent(const composer::project::MidiEvent& event, const juce::MidiMessage& message, double time)
{
    CHECK(event.timeSeconds == time);
    REQUIRE(event.bytes.size() == static_cast<std::size_t>(message.getRawDataSize()));
    for (std::size_t i = 0; i < event.bytes.size(); ++i)
        CHECK(event.bytes[i] == message.getRawData()[i]);
}
}

TEST_CASE("Recording capture preserves arrival order with nondecreasing mapped times")
{
    ClockFixture clock;
    clock.mapping.streamToEditOffset = 96;
    clock.mapping.takeStartEditSample = 48000;
    RecordingCapture capture(clock.signals, clock.mapping, 8, 0, 100);
    CHECK_THROWS_AS(capture.collect(), std::logic_error);
    REQUIRE(capture.open(0));
    CHECK_FALSE(capture.open(0));
    CHECK(capture.accepting());

    const auto later = note(71, 1.0 + 17.0 / 48000.0);
    const auto first = note(70, 1.0);
    const std::array<std::uint8_t, 2> programBytes{0xc3, 117};
    const juce::MidiMessage equal(programBytes.data(), 2, 1.25);
    capture.handleIncomingMidiMessage(nullptr, later);
    capture.handleIncomingMidiMessage(nullptr, first);
    clock.signals.correction.store(-0.25);
    capture.handleIncomingMidiMessage(nullptr, equal);
    capture.close();
    capture.handleIncomingMidiMessage(nullptr, first);
    CHECK_FALSE(capture.open(1));
    capture.sealAfterJoin(1);

    const auto& take = capture.collect();
    REQUIRE(take.isComplete());
    REQUIRE(take.captured.events.size() == 3);
    checkEvent(take.captured.events[0], later, 113.0 / 48000.0);
    checkEvent(take.captured.events[1], first, 113.0 / 48000.0);
    checkEvent(take.captured.events[2], equal, 113.0 / 48000.0);
    CHECK(take.arrivalTimeAdjustments == 2);
    CHECK(take.preOriginEvents == 0);
    CHECK(&capture.collect() == &take);
}

TEST_CASE("A decreasing block correction cannot invert a same-timestamp note release and repeat")
{
    ClockFixture clock;
    const auto correction = 12.0 - 259200.0;
    clock.signals.correction.store(correction);
    RecordingCapture capture(clock.signals, clock.mapping, 4, 0, 100);
    REQUIRE(capture.open(0));
    const auto original = note(60, 259200.05);
    auto release = juce::MidiMessage::noteOff(3, 60, static_cast<juce::uint8>(64));
    release.setTimeStamp(259200.25);
    const auto repeat = note(60, release.getTimeStamp());
    const auto later = note(61, 259200.5);
    capture.handleIncomingMidiMessage(nullptr, original);
    capture.handleIncomingMidiMessage(nullptr, release);
    clock.signals.serial.store(11);
    clock.signals.correction.store(correction - 0.0004);
    capture.handleIncomingMidiMessage(nullptr, repeat);
    capture.handleIncomingMidiMessage(nullptr, later);
    capture.sealAfterJoin(1);
    const auto& take = capture.collect();
    REQUIRE(take.isComplete());
    REQUIRE(take.captured.events.size() == 4);
    checkEvent(take.captured.events[0], original, 12.05);
    checkEvent(take.captured.events[1], release, 12.25);
    checkEvent(take.captured.events[2], repeat, 12.25);
    const auto laterSamples = static_cast<std::int64_t>((later.getTimeStamp() + correction - 0.0004) * 48000.0 + 0.5);
    checkEvent(take.captured.events[3], later, static_cast<double>(laterSamples) / 48000.0);
    CHECK(take.arrivalTimeAdjustments == 1);
    CHECK(take.preOriginEvents == 0);
    CHECK(&take == &capture.collect());
    CHECK(capture.collect().arrivalTimeAdjustments == 1);
}

TEST_CASE("Recording capture projects a regressed raw timestamp without changing an ordered prefix")
{
    ClockFixture clock;
    RecordingCapture capture(clock.signals, clock.mapping, 5, 0, 100);
    REQUIRE(capture.open(0));
    const std::array messages {note(60, 0.125), note(61, 0.25), note(62, 0.25), note(63, 0.1), note(64, 0.5)};
    for (const auto& message : messages) capture.handleIncomingMidiMessage(nullptr, message);
    capture.sealAfterJoin(1);
    const auto& take = capture.collect();
    REQUIRE(take.isComplete());
    REQUIRE(take.captured.events.size() == messages.size());
    const std::array times {0.125, 0.25, 0.25, 0.25, 0.5};
    for (std::size_t i = 0; i < messages.size(); ++i) checkEvent(take.captured.events[i], messages[i], times[i]);
    CHECK(take.arrivalTimeAdjustments == 1);
    CHECK(take.preOriginEvents == 0);
}

TEST_CASE("Recording capture retains valid pre-origin messages and counts only accepted channel events")
{
    ClockFixture clock;
    clock.mapping.takeStartEditSample = 48000;
    SECTION("A delayed valid message remains complete at the origin")
    {
        RecordingCapture capture(clock.signals, clock.mapping, 2, 0, 100);
        REQUIRE(capture.open(0));
        const auto early = note(60, 0.5);
        const auto atOrigin = note(61, 1.0);
        capture.handleIncomingMidiMessage(nullptr, early);
        capture.handleIncomingMidiMessage(nullptr, atOrigin);
        capture.sealAfterJoin(1);
        const auto& take = capture.collect();
        REQUIRE(take.isComplete());
        REQUIRE(take.captured.events.size() == 2);
        checkEvent(take.captured.events[0], early, 0.0);
        checkEvent(take.captured.events[1], atOrigin, 0.0);
        CHECK(take.preOriginEvents == 1);
        CHECK(take.arrivalTimeAdjustments == 0);
    }
    SECTION("Invalid ignored and overflowing inputs do not increment the projection count")
    {
        RecordingCapture capture(clock.signals, clock.mapping, 1, 0, 100);
        REQUIRE(capture.open(0));
        const std::array<std::uint8_t, 3> malformedBytes {0x90, 128, 93};
        const juce::MidiMessage malformed(malformedBytes.data(), 3, 0.5);
        capture.handleIncomingMidiMessage(nullptr, malformed);
        auto clockMessage = juce::MidiMessage::midiClock();
        clockMessage.setTimeStamp(0.5);
        capture.handleIncomingMidiMessage(nullptr, clockMessage);
        capture.handleIncomingMidiMessage(nullptr, note(60, -0.1));
        capture.handleIncomingMidiMessage(nullptr, note(60, std::numeric_limits<double>::quiet_NaN()));
        const auto accepted = note(61, 0.5);
        capture.handleIncomingMidiMessage(nullptr, accepted);
        capture.handleIncomingMidiMessage(nullptr, note(62, 0.75));
        capture.sealAfterJoin(1);
        const auto& take = capture.collect();
        REQUIRE(take.captured.events.size() == 1);
        checkEvent(take.captured.events[0], accepted, 0.0);
        CHECK(take.preOriginEvents == 1);
        CHECK(take.arrivalTimeAdjustments == 0);
        CHECK(take.captured.invalidTimestamps == 2);
        CHECK(take.captured.invalidMessages == 1);
        CHECK(take.captured.ignoredSystemMessages == 1);
        CHECK(take.captured.overflowEvents == 1);
        CHECK_FALSE(take.isComplete());
    }
    SECTION("A late pre-origin delivery also preserves the preceding arrival time")
    {
        RecordingCapture capture(clock.signals, clock.mapping, 2, 0, 100);
        REQUIRE(capture.open(0));
        const auto first = note(60, 2.0);
        const auto delayed = note(61, 0.5);
        capture.handleIncomingMidiMessage(nullptr, first);
        capture.handleIncomingMidiMessage(nullptr, delayed);
        capture.sealAfterJoin(1);
        const auto& take = capture.collect();
        REQUIRE(take.isComplete());
        REQUIRE(take.captured.events.size() == 2);
        checkEvent(take.captured.events[0], first, 1.0);
        checkEvent(take.captured.events[1], delayed, 1.0);
        CHECK(take.preOriginEvents == 1);
        CHECK(take.arrivalTimeAdjustments == 1);
    }
}

TEST_CASE("Recording capture validates preparation before admitting callbacks")
{
    ClockFixture clock;
    auto invalid = clock.mapping;
    invalid.generation = 0;
    CHECK_THROWS_AS(RecordingCapture(clock.signals, invalid, 1, 0, 100), std::invalid_argument);
    invalid.generation = std::numeric_limits<std::uint64_t>::max();
    CHECK_THROWS_AS(RecordingCapture(clock.signals, invalid, 1, 0, 100), std::invalid_argument);
    invalid = clock.mapping;
    invalid.proofSerial = 0;
    CHECK_THROWS_AS(RecordingCapture(clock.signals, invalid, 1, 0, 100), std::invalid_argument);
    for (const auto rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()})
    {
        invalid = clock.mapping;
        invalid.sampleRate = rate;
        CHECK_THROWS_AS(RecordingCapture(clock.signals, invalid, 1, 0, 100), std::invalid_argument);
    }
    invalid = clock.mapping;
    invalid.takeStartEditSample = -1;
    CHECK_THROWS_AS(RecordingCapture(clock.signals, invalid, 1, 0, 100), std::invalid_argument);
    invalid.takeStartEditSample = 1;
    invalid.streamToEditOffset = std::numeric_limits<std::int64_t>::min();
    CHECK_THROWS_AS(RecordingCapture(clock.signals, invalid, 1, 0, 100), std::invalid_argument);
    CHECK_THROWS_AS(RecordingCapture(clock.signals, clock.mapping, 0, 0, 100), std::invalid_argument);
    CHECK_THROWS_AS(RecordingCapture(clock.signals, clock.mapping, composer::project::maxProjectEvents + 1, 0, 100),
                    std::invalid_argument);
    CHECK_THROWS_AS(RecordingCapture(clock.signals, clock.mapping, 1, 0, 0), std::invalid_argument);

    RecordingCapture neverOpened(clock.signals, clock.mapping, 1, 0, 100);
    neverOpened.handleIncomingMidiMessage(nullptr, note(60, 0.0));
    neverOpened.close();
    CHECK_FALSE(neverOpened.open(0));
    neverOpened.sealAfterJoin(1);
    CHECK(neverOpened.collect().captured.events.empty());
    CHECK_FALSE(neverOpened.collect().isComplete());
    CHECK(hasFault(neverOpened, CaptureInterruption::neverAdmitted));

    RecordingCapture valid(clock.signals, clock.mapping, 1, 0, 100);
    CHECK_THROWS_AS(composer::app::RecordingInput(nullptr, nullptr, valid), std::invalid_argument);
    REQUIRE(valid.open(0));
    valid.sealAfterJoin(1);
    CHECK(valid.collect().isComplete());
}

TEST_CASE("Recording capture latches generation changes before admission and without MIDI")
{
    SECTION("The prepared generation is already obsolete")
    {
        ClockFixture clock;
        RecordingCapture capture(clock.signals, clock.mapping, 1, 0, 100);
        clock.signals.generation.store(2);
        CHECK_FALSE(capture.open(0));
        capture.sealAfterJoin(1);
        CHECK(hasFault(capture, CaptureInterruption::generationChanged));
        CHECK(hasFault(capture, CaptureInterruption::neverAdmitted));
        CHECK_FALSE(capture.collect().isComplete());
    }
    SECTION("A callback checks the generation again after successful open")
    {
        ClockFixture clock;
        RecordingCapture capture(clock.signals, clock.mapping, 1, 0, 100);
        REQUIRE(capture.open(0));
        clock.signals.generation.store(2);
        capture.handleIncomingMidiMessage(nullptr, note(60, 0.0));
        CHECK_FALSE(capture.accepting());
        capture.sealAfterJoin(1);
        CHECK(capture.collect().captured.events.empty());
        CHECK(hasFault(capture, CaptureInterruption::generationChanged));
    }
    SECTION("The final owner check detects a change when no callback arrives")
    {
        ClockFixture clock;
        RecordingCapture capture(clock.signals, clock.mapping, 1, 0, 100);
        REQUIRE(capture.open(0));
        clock.signals.generation.store(2);
        capture.close();
        capture.sealAfterJoin(1);
        CHECK(hasFault(capture, CaptureInterruption::generationChanged));
        CHECK_FALSE(capture.collect().isComplete());
    }
}

TEST_CASE("Recording capture requires fresh publication and a timely owner poll")
{
    SECTION("A stationary publisher expires despite frequent owner checks")
    {
        ClockFixture clock;
        RecordingCapture capture(clock.signals, clock.mapping, 1, 0, 100);
        REQUIRE(capture.open(0));
        CHECK(capture.poll(50));
        CHECK(capture.poll(99));
        CHECK_FALSE(capture.poll(100));
        CHECK(hasFault(capture, CaptureInterruption::stalePublisher));
        CHECK_FALSE(hasFault(capture, CaptureInterruption::lateOwnerPoll));
        capture.sealAfterJoin(100);
        CHECK_FALSE(capture.collect().isComplete());
    }
    SECTION("Publication cannot excuse a missing or backwards owner poll")
    {
        ClockFixture clock;
        RecordingCapture capture(clock.signals, clock.mapping, 1, 10, 100);
        REQUIRE(capture.open(10));
        clock.signals.serial.store(11);
        CHECK_FALSE(capture.poll(110));
        CHECK(hasFault(capture, CaptureInterruption::lateOwnerPoll));
        capture.sealAfterJoin(109);
        CHECK_FALSE(capture.collect().isComplete());
    }
    SECTION("Regressed serials and invalid corrections latch interruption")
    {
        ClockFixture clock;
        RecordingCapture capture(clock.signals, clock.mapping, 1, 0, 100);
        REQUIRE(capture.open(0));
        clock.signals.serial.store(9);
        clock.signals.correction.store(std::numeric_limits<double>::quiet_NaN());
        CHECK_FALSE(capture.poll(1));
        CHECK(hasFault(capture, CaptureInterruption::regressedSerial));
        CHECK(hasFault(capture, CaptureInterruption::invalidCorrection));
        clock.signals.serial.store(12);
        clock.signals.correction.store(0.0);
        CHECK_FALSE(capture.poll(2));
        capture.sealAfterJoin(2);
        CHECK_FALSE(capture.collect().isComplete());
    }
}

TEST_CASE("Recording capture rejects invalid mapping results and reports bounded loss")
{
    ClockFixture clock;
    RecordingCapture capture(clock.signals, clock.mapping, 1, 0, 100);
    REQUIRE(capture.open(0));
    for (const auto time : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max(),
                            9223372036854775808.0 / 48000.0,
                            composer::project::maxProjectDurationSeconds + 1.0})
        capture.handleIncomingMidiMessage(nullptr, note(60, time));
    clock.signals.correction.store(-2.0);
    capture.handleIncomingMidiMessage(nullptr, note(60, 1.0));
    clock.signals.correction.store(0.0);
    const std::array<std::uint8_t, 3> invalidBytes{0x90, 128, 93};
    const juce::MidiMessage invalid(invalidBytes.data(), 3, 0.0);
    capture.handleIncomingMidiMessage(nullptr, invalid);
    capture.handleIncomingMidiMessage(nullptr, juce::MidiMessage::midiClock());
    const auto accepted = note(61, 0.0);
    capture.handleIncomingMidiMessage(nullptr, accepted);
    capture.handleIncomingMidiMessage(nullptr, note(62, 0.1));
    capture.sealAfterJoin(1);
    const auto& take = capture.collect();
    REQUIRE(take.captured.events.size() == 1);
    checkEvent(take.captured.events[0], accepted, 0.0);
    CHECK(take.captured.invalidTimestamps == 7);
    CHECK(take.captured.invalidMessages == 1);
    CHECK(take.captured.ignoredSystemMessages == 1);
    CHECK(take.captured.overflowEvents == 1);
    CHECK_FALSE(take.isComplete());
}

TEST_CASE("Recording capture uses literal zero and the positive sample rounding boundary")
{
    ClockFixture clock;
    clock.mapping.sampleRate = 2.0;
    RecordingCapture capture(clock.signals, clock.mapping, 3, 0, 100);
    REQUIRE(capture.open(0));
    const auto below = note(60, std::nextafter(0.25, 0.0));
    const auto at = note(61, 0.25);
    const auto zero = note(62, 0.0);
    capture.handleIncomingMidiMessage(nullptr, zero);
    capture.handleIncomingMidiMessage(nullptr, below);
    capture.handleIncomingMidiMessage(nullptr, at);
    capture.sealAfterJoin(1);
    const auto& events = capture.collect().captured.events;
    REQUIRE(events.size() == 3);
    // The multiply-plus-half itself rounds in double precision, just as Tracktion does.
    const auto roundedBelow = static_cast<std::int64_t>(below.getTimeStamp() * 2.0 + 0.5);
    CHECK(events[0].timeSeconds == 0.0);
    CHECK(events[1].timeSeconds == static_cast<double>(roundedBelow) / 2.0);
    CHECK(events[2].timeSeconds == 0.5);
    CHECK(capture.collect().arrivalTimeAdjustments == 0);
    CHECK(capture.collect().preOriginEvents == 0);

    auto overflowMapping = clock.mapping;
    overflowMapping.streamToEditOffset = std::numeric_limits<std::int64_t>::max();
    RecordingCapture overflow(clock.signals, overflowMapping, 1, 0, 100);
    REQUIRE(overflow.open(0));
    overflow.handleIncomingMidiMessage(nullptr, note(60, 1.0));
    overflow.sealAfterJoin(1);
    CHECK(overflow.collect().captured.invalidTimestamps == 1);
}

TEST_CASE("Sealed recording capture survives publisher destruction and preserves completeness")
{
    auto signals = std::make_unique<CaptureClockSignals>();
    signals->generation.store(1);
    signals->serial.store(10);
    signals->correction.store(0.0);
    RecordingCapture capture(*signals, {1, 10, 48000.0, 0, 0}, 1, 0, 100);
    REQUIRE(capture.open(0));
    const auto original = note(63, 0.125);
    capture.handleIncomingMidiMessage(nullptr, original);
    capture.sealAfterJoin(1);
    signals->generation.store(2);
    signals->correction.store(std::numeric_limits<double>::quiet_NaN());
    signals.reset();
    CHECK(capture.poll(10000));
    capture.sealAfterJoin(10000, static_cast<unsigned>(CaptureInterruption::ownerAbandoned));
    const auto& take = capture.collect();
    REQUIRE(take.isComplete());
    REQUIRE(take.captured.events.size() == 1);
    checkEvent(take.captured.events[0], original, 0.125);
    CHECK(&take == &capture.collect());

    ClockFixture otherClock;
    RecordingCapture abandoned(otherClock.signals, otherClock.mapping, 1, 0, 100);
    REQUIRE(abandoned.open(0));
    abandoned.handleIncomingMidiMessage(nullptr, original);
    abandoned.sealAfterJoin(1, static_cast<unsigned>(CaptureInterruption::ownerAbandoned));
    CHECK_FALSE(abandoned.collect().isComplete());
    CHECK(abandoned.collect().captured.events == take.captured.events);
}

TEST_CASE("Joined callback producers preserve originals across a single-use take boundary")
{
    ClockFixture clock;
    RecordingCapture first(clock.signals, clock.mapping, 256, 0, 100);
    REQUIRE(first.open(0));
    std::array<std::thread, 4> producers;
    for (std::size_t producer = 0; producer < producers.size(); ++producer)
        producers[producer] = std::thread([&, producer]
        {
            for (int i = 0; i < 64; ++i)
            {
                juce::MidiMessage message(0x90 | static_cast<int>(producer), i, 91);
                message.setTimeStamp(static_cast<double>(producer * 64 + i) / 48000.0);
                first.handleIncomingMidiMessage(nullptr, message);
            }
        });
    for (auto& producer : producers) producer.join();
    first.close();
    first.sealAfterJoin(1);
    const auto& oldTake = first.collect();
    REQUIRE(oldTake.isComplete());
    REQUIRE(oldTake.captured.events.size() == 256);
    std::array<int, 4> counts{};
    double previous = 0.0;
    std::uint64_t projected = 0;
    for (const auto& event : oldTake.captured.events)
    {
        const auto producer = static_cast<std::size_t>(event.bytes[0] & 0x0f);
        REQUIRE(producer < counts.size());
        REQUIRE(counts[producer] < 64);
        const auto i = counts[producer]++;
        CHECK(event.bytes[1] == static_cast<std::uint8_t>(i));
        const auto mapped = static_cast<double>(producer * 64 + static_cast<std::size_t>(i)) / 48000.0;
        if (mapped < previous) ++projected;
        else previous = mapped;
        CHECK(event.timeSeconds == previous);
    }
    for (auto count : counts) CHECK(count == 64);
    CHECK(oldTake.arrivalTimeAdjustments == projected);
    clock.signals.generation.store(2);
    clock.mapping.generation = 2;
    RecordingCapture second(clock.signals, clock.mapping, 1, 2, 100);
    REQUIRE(second.open(2));
    first.handleIncomingMidiMessage(nullptr, note(77, 0.0));
    second.handleIncomingMidiMessage(nullptr, note(78, 0.0));
    second.sealAfterJoin(3);
    CHECK(first.collect().captured.events.size() == 256);
    REQUIRE(second.collect().captured.events.size() == 1);
    CHECK(second.collect().captured.events[0].bytes[1] == 78);
}

TEST_CASE("The raw recording callback performs no heap operations")
{
    if (!composer::tests::AllocationProbe::available()) SKIP("Requires the Debug CRT allocation hook");
    using Probe = composer::tests::AllocationProbe;
    void* (*volatile allocate)(std::size_t) = &std::malloc;
    Probe::arm();
    auto* memory = allocate(32);
    std::free(memory);
    CHECK(Probe::disarm() >= 2);

    ClockFixture clock;
    RecordingCapture capture(clock.signals, clock.mapping, 1024, 0, 100);
    REQUIRE(capture.open(0));
    const auto message = note(60, 0.0);
    Probe::arm();
    for (int i = 0; i < 1024; ++i) capture.handleIncomingMidiMessage(nullptr, message);
    const auto operations = Probe::disarm();
    CHECK(operations == 0);
    capture.sealAfterJoin(1);
    REQUIRE(capture.collect().isComplete());
    CHECK(capture.collect().captured.events.size() == 1024);

    clock.mapping.takeStartEditSample = 1;
    RecordingCapture delayed(clock.signals, clock.mapping, 1024, 0, 100);
    REQUIRE(delayed.open(0));
    Probe::arm();
    for (int i = 0; i < 1024; ++i) delayed.handleIncomingMidiMessage(nullptr, message);
    const auto delayedOperations = Probe::disarm();
    CHECK(delayedOperations == 0);
    delayed.sealAfterJoin(1);
    REQUIRE(delayed.collect().isComplete());
    CHECK(delayed.collect().preOriginEvents == 1024);
    CHECK(delayed.collect().arrivalTimeAdjustments == 0);
}
