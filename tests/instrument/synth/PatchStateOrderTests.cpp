#include "SynthTestSupport.h"

#include <composer/instrument/synth/ParameterCurves.h>
#include <composer/instrument/synth/PatchState.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace composer::instrument::synth
{

/** Runs the private steps of publishes, host writes and reads one at a time, so a test can
    place them in any order on one thread. */
struct PatchStateSteps
{
    using Publication = PatchState::Publication;
    using HostWrite = PatchState::HostWrite;

    static void beginPublish(PatchState& state, Publication& publication, const contracts::InstrumentPatch& patch)
    {
        state.beginPublish(publication, patch);
    }

    static void switchGeneration(PatchState& state, const Publication& publication)
    {
        state.switchGeneration(publication);
    }

    static std::bitset<parameterCount> finishPublish(PatchState& state, Publication& publication)
    {
        return state.finishPublish(publication);
    }

    static HostWrite beginHostWrite(PatchState& state, std::size_t index, float value)
    {
        return state.beginHostWrite(index, value);
    }

    static bool attemptHostWrite(PatchState& state, HostWrite& write)
    {
        return state.attemptHostWrite(write);
    }

    template <typename BetweenLoads>
    static void prepareHostWrite(const PatchState& state, HostWrite& write, BetweenLoads&& betweenLoads)
    {
        state.prepareHostWrite(write, betweenLoads);
    }

    static void finishHostWrite(PatchState& state)
    {
        state.finishHostWrite();
    }

    template <typename BeforeAttempt>
    static void writeHostValue(PatchState& state, std::size_t index, float value, BeforeAttempt&& beforeAttempt)
    {
        state.writeHostValue(index, value, beforeAttempt);
    }

    template <typename AfterLoad>
    static bool readPatch(const PatchState& state, contracts::InstrumentPatch& out, AfterLoad&& afterLoad)
    {
        return state.readPatch(out, afterLoad);
    }

    template <typename Interrupt>
    static float readHostValue(const PatchState& state, std::size_t index, Interrupt&& interrupt)
    {
        return state.readHostValue(index, interrupt);
    }
};

} // namespace composer::instrument::synth

namespace
{

using namespace composer;
using namespace composer::instrument::synth;
using Steps = PatchStateSteps;

std::size_t indexOf(std::string_view id)
{
    std::size_t index = 0;

    while (contracts::parameterDescriptors[index].id != id)
        ++index;

    return index;
}

const std::size_t gain = indexOf("gain_db");
const std::size_t cutoff = indexOf("cutoff_hz");
const std::size_t sustain = indexOf("sustain_level");

// Every patch has the same gain and cutoff, so the host is told about either only when a
// publish replaced a host value. Sustain tells the patches apart.
constexpr std::array<double, 3> sustainOfPatch { 0.7, 0.3, 0.5 };

contracts::InstrumentPatch patchNumber(int number)
{
    return testing::makePatch(contracts::Waveform::saw, -12.0, 0.01, 0.1, sustainOfPatch[static_cast<std::size_t>(number)],
                              0.2, 8000.0, 0.7071067811865476);
}

/** Host settings that differ from the patches' gain (0.8) and cutoff (about 0.867). */
constexpr std::array<float, 3> hostSetting { 0.0f, 0.2f, 0.9f };
constexpr float hostCutoff = 0.25f;

/** What a read shows: which patch, and which host value, if any, each parameter carries. */
struct Observed
{
    int patch = 0;
    int gainFrom = 0;
    int cutoffFrom = 0;

    bool operator==(const Observed&) const = default;
};

Observed classify(const contracts::InstrumentPatch& patch)
{
    Observed observed;
    const auto found = std::find(sustainOfPatch.begin(), sustainOfPatch.end(), patch.sustainLevel);
    REQUIRE(found != sustainOfPatch.end());
    observed.patch = static_cast<int>(found - sustainOfPatch.begin());

    for (int source = 1; source < 3; ++source)
        if (patch.gainDb == denormalise(gain, hostSetting[static_cast<std::size_t>(source)]))
            observed.gainFrom = source;

    REQUIRE((observed.gainFrom != 0 || patch.gainDb == -12.0));

    observed.cutoffFrom = patch.cutoffHz == denormalise(cutoff, hostCutoff) ? 1 : 0;
    REQUIRE((observed.cutoffFrom != 0 || patch.cutoffHz == 8000.0));
    return observed;
}

/** One operation of a scenario: a publish of a patch, or a host write of a parameter. */
struct Operation
{
    bool isPublish = false;
    int patch = 0;
    std::size_t parameter = 0;
    int source = 0;
    float setting = 0.0f;
};

Observed apply(Observed state, const Operation& operation)
{
    if (operation.isPublish)
        return { operation.patch, 0, 0 };

    if (operation.parameter == gain)
        state.gainFrom = operation.source;
    else
        state.cutoffFrom = operation.source;

    return state;
}

/** A thread runs its operations in order. A scenario runs its threads, and any slow readers, in
    every interleaving of their steps. A slow reader's patch read takes two steps: it begins and
    loads the parameters up to and including its pause index, then loads the rest and checks. A
    reader may begin and finish while another is paused; schedules are limited to that nesting. */
using Program = std::vector<int>;

struct Scenario
{
    std::vector<Operation> operations;
    std::vector<Program> threads;
    std::vector<std::size_t> readerPauses;
};

/** A successful patch read: when it began and ended, as step positions, and what it showed. A
    read made between steps k and k + 1 begins and ends at k + 0.5. */
struct Read
{
    double first = 0.0;
    double last = 0.0;
    Observed observed;
};

/** The record of one schedule: what each successful read showed, when each operation began
    and ended, and what each publish reported. */
struct Run
{
    std::vector<Read> reads;
    std::vector<int> begins;
    std::vector<int> ends;
    std::vector<std::bitset<parameterCount>> reports;
    Observed final;
    std::vector<int> runnable;
};

struct ThreadState
{
    std::size_t operation = 0;
    int stage = 0;
    Steps::Publication publication;
    Steps::HostWrite write;
    int attempts = 0;
};

constexpr int slowReaderSteps = 2;

/** Replays one schedule, a list of actor numbers: the threads, then the slow readers. A slow
    reader's second step runs from inside its own patch read, between parameter loads, so the
    other actors' steps, including whole reads by other slow readers, fall between them. A quick
    read is attempted after every step. */
class Replayer
{
public:
    Replayer(const Scenario& scenarioToRun, const std::vector<int>& scheduleToRun)
        : scenario(scenarioToRun),
          schedule(scheduleToRun),
          state(patchNumber(0)),
          threads(scenarioToRun.threads.size()),
          readerSteps(scenarioToRun.readerPauses.size(), 0)
    {
        run.begins.assign(scenario.operations.size(), -1);
        run.ends.assign(scenario.operations.size(), -1);
        run.reports.resize(scenario.operations.size());
    }

    Run replay()
    {
        readAfter(-1);
        runUntil(-1);

        if (run.runnable.empty())
            run.final = classify(state.snapshot());

        return std::move(run);
    }

private:
    int readerActor(std::size_t reader) const
    {
        return static_cast<int>(threads.size() + reader);
    }

    void readAfter(int step)
    {
        if (exhausted)
            return;

        contracts::InstrumentPatch patch;

        if (state.tryRead(patch))
            run.reads.push_back({ step + 0.5, step + 0.5, classify(patch) });
    }

    /** Runs scheduled steps until the schedule ends or it is the given actor's turn. */
    void runUntil(int waitingFor)
    {
        while (cursor < schedule.size())
        {
            if (schedule[cursor] == waitingFor)
                return;

            const int actor = schedule[cursor++];
            const int step = steps++;

            if (actor >= static_cast<int>(threads.size()))
                slowRead(static_cast<std::size_t>(actor) - threads.size(), step);
            else
                threadStep(actor, step);

            // A slow read takes its second step inside, so the latest step may be that one.
            readAfter(steps - 1);
        }

        noteRunnable();
    }

    /** Takes a paused reader's next turn, if the schedule reaches it. */
    bool takeReaderTurn(std::size_t reader, int& step)
    {
        runUntil(readerActor(reader));

        if (exhausted || cursor >= schedule.size())
            return false;

        ++cursor;
        step = steps++;
        ++readerSteps[reader];
        return true;
    }

    void slowRead(std::size_t reader, int firstStep)
    {
        REQUIRE(readerSteps[reader] == 0);
        readerSteps[reader] = 1;
        paused.push_back(reader);

        const auto pause = scenario.readerPauses[reader];
        int lastStep = firstStep;
        bool complete = true;
        contracts::InstrumentPatch patch;

        const bool accepted = Steps::readPatch(state, patch, [&](std::size_t index) {
            if (index != pause || ! complete)
                return;

            readAfter(lastStep);
            complete = takeReaderTurn(reader, lastStep);
        });

        // A read that fails at its start check takes its remaining turn doing nothing, so every
        // schedule gives each reader the same number of steps.
        if (complete && readerSteps[reader] < slowReaderSteps)
        {
            readAfter(lastStep);
            complete = takeReaderTurn(reader, lastStep);
        }

        paused.pop_back();

        if (accepted && complete)
            run.reads.push_back({ static_cast<double>(firstStep), static_cast<double>(lastStep), classify(patch) });
    }

    void threadStep(int thread, int step)
    {
        auto& running = threads[static_cast<std::size_t>(thread)];
        const int id = scenario.threads[static_cast<std::size_t>(thread)][running.operation];
        const auto& operation = scenario.operations[static_cast<std::size_t>(id)];

        if (running.stage == 0)
            run.begins[static_cast<std::size_t>(id)] = step;

        bool finished = false;

        if (operation.isPublish)
        {
            if (running.stage == 0)
                Steps::beginPublish(state, running.publication, patchNumber(operation.patch));
            else if (running.stage == 1)
                Steps::switchGeneration(state, running.publication);
            else
            {
                run.reports[static_cast<std::size_t>(id)] = Steps::finishPublish(state, running.publication);
                finished = true;
            }

            ++running.stage;
        }
        else if (running.stage == 0)
        {
            running.write = Steps::beginHostWrite(state, operation.parameter, operation.setting);
            running.attempts = 0;
            running.stage = 1;
        }
        else if (running.stage == 1)
        {
            ++running.attempts;

            if (Steps::attemptHostWrite(state, running.write) || running.attempts == PatchState::maximumHostWriteAttempts)
                running.stage = 2;
        }
        else
        {
            Steps::finishHostWrite(state);
            finished = true;
        }

        if (finished)
        {
            run.ends[static_cast<std::size_t>(id)] = step;
            running = ThreadState { running.operation + 1 };
        }
    }

    /** Records which actors could take the next step when the schedule ends, once. A paused
        reader can resume only if no reader paused after it is still paused. */
    void noteRunnable()
    {
        if (exhausted)
            return;

        exhausted = true;

        for (std::size_t thread = 0; thread < threads.size(); ++thread)
            if (threads[thread].operation < scenario.threads[thread].size())
                run.runnable.push_back(static_cast<int>(thread));

        for (std::size_t reader = 0; reader < readerSteps.size(); ++reader)
            if (readerSteps[reader] == 0 || (readerSteps[reader] < slowReaderSteps && ! paused.empty() && paused.back() == reader))
                run.runnable.push_back(readerActor(reader));
    }

    const Scenario& scenario;
    const std::vector<int>& schedule;
    PatchState state;
    std::vector<ThreadState> threads;
    std::vector<int> readerSteps;
    std::vector<std::size_t> paused;
    Run run;
    std::size_t cursor = 0;
    int steps = 0;
    bool exhausted = false;
};

/** Whether one serial order of the operations explains the run. The order keeps each thread's
    order and puts an operation that ended before another began first. Each successful read
    shows a prefix of it holding every operation that ended before the read began and none that
    began after the read ended, and a read that ended before another began shows no longer a
    prefix. The final state is the whole order. Each publish reports a parameter only when it
    replaced a host value, and always does unless a later host write has already taken the
    publish's place. */
bool explains(const Scenario& scenario, const Run& run, const std::vector<int>& order)
{
    const auto count = order.size();
    std::vector<std::size_t> position(count);
    for (std::size_t index = 0; index < count; ++index)
        position[static_cast<std::size_t>(order[index])] = index;

    for (const auto& program : scenario.threads)
        for (std::size_t index = 1; index < program.size(); ++index)
            if (position[static_cast<std::size_t>(program[index - 1])] > position[static_cast<std::size_t>(program[index])])
                return false;

    for (std::size_t a = 0; a < count; ++a)
        for (std::size_t b = 0; b < count; ++b)
            if (run.ends[a] < run.begins[b] && position[a] > position[b])
                return false;

    std::vector<Observed> states { Observed {} };
    for (const int id : order)
        states.push_back(apply(states.back(), scenario.operations[static_cast<std::size_t>(id)]));

    for (std::size_t index = 0; index < count; ++index)
    {
        const auto& operation = scenario.operations[static_cast<std::size_t>(order[index])];

        if (! operation.isPublish)
            continue;

        const auto& before = states[index];
        const auto& report = run.reports[static_cast<std::size_t>(order[index])];

        if (! report.test(sustain))
            return false;

        for (const auto parameter : { gain, cutoff })
        {
            const bool replaced = (parameter == gain ? before.gainFrom : before.cutoffFrom) != 0;
            bool overwritten = false;

            for (std::size_t later = index + 1; later < count; ++later)
            {
                const auto& next = scenario.operations[static_cast<std::size_t>(order[later])];

                if (next.isPublish)
                    break;

                overwritten = overwritten || next.parameter == parameter;
            }

            if (report.test(parameter) != replaced && ! (replaced && overwritten))
                return false;
        }
    }

    // Reads in the order they began; each takes the shortest prefix it can.
    std::vector<std::size_t> byStart(run.reads.size());
    std::iota(byStart.begin(), byStart.end(), 0);
    std::stable_sort(byStart.begin(), byStart.end(),
                     [&](std::size_t a, std::size_t b) { return run.reads[a].first < run.reads[b].first; });
    std::vector<std::size_t> chosen(run.reads.size(), 0);

    for (std::size_t rank = 0; rank < byStart.size(); ++rank)
    {
        const auto& read = run.reads[byStart[rank]];
        std::size_t lowest = 0;

        for (std::size_t earlier = 0; earlier < rank; ++earlier)
            if (run.reads[byStart[earlier]].last < read.first)
                lowest = std::max(lowest, chosen[byStart[earlier]]);

        bool found = false;

        for (std::size_t candidate = lowest; candidate <= count && ! found; ++candidate)
        {
            bool fits = states[candidate] == read.observed;

            for (std::size_t index = 0; index < count && fits; ++index)
            {
                const auto id = static_cast<std::size_t>(order[index]);
                const bool inPrefix = index < candidate;

                if ((run.ends[id] < read.first && ! inPrefix) || (run.begins[id] > read.last && inPrefix))
                    fits = false;
            }

            if (fits)
            {
                chosen[byStart[rank]] = candidate;
                found = true;
            }
        }

        if (! found)
            return false;
    }

    return states.back() == run.final;
}

/** Runs every schedule of a scenario and checks each against every serial order. Returns the
    number of schedules and, through acceptedSlowReads, how many slow reads succeeded. */
int checkEverySchedule(const Scenario& scenario, int* acceptedSlowReads = nullptr)
{
    int schedules = 0;
    std::vector<int> order(scenario.operations.size());
    std::vector<int> schedule;

    const std::function<void()> explore = [&] {
        const auto run = Replayer(scenario, schedule).replay();

        if (! run.runnable.empty())
        {
            for (const int actor : run.runnable)
            {
                schedule.push_back(actor);
                explore();
                schedule.pop_back();
            }

            return;
        }

        ++schedules;

        if (acceptedSlowReads != nullptr)
            for (const auto& read : run.reads)
                *acceptedSlowReads += read.first != read.last ? 1 : 0;

        std::iota(order.begin(), order.end(), 0);
        bool explained = false;

        do
            explained = explains(scenario, run, order);
        while (! explained && std::next_permutation(order.begin(), order.end()));

        if (! explained)
        {
            std::string text;
            for (const int actor : schedule)
                text += std::to_string(actor);

            FAIL_CHECK("no serial order explains schedule " << text);
        }
    };

    explore();
    return schedules;
}

} // namespace

TEST_CASE("Every interleaving of a publish with two ordered host edits fits one serial order")
{
    // One host thread edits gain then cutoff while a publish runs.
    const Scenario scenario {
        { { true, 1 }, { false, 0, gain, 1, hostSetting[1] }, { false, 0, cutoff, 1, hostCutoff } },
        { { 0 }, { 1, 2 } },
    };

    CHECK(checkEverySchedule(scenario) > 0);
}

TEST_CASE("Every interleaving of a publish with two host threads writing one parameter fits one serial order")
{
    // Thread 1 writes gain; thread 2 writes gain then cutoff; a publish runs alongside.
    const Scenario scenario {
        { { true, 1 }, { false, 0, gain, 1, hostSetting[1] }, { false, 0, gain, 2, hostSetting[2] },
          { false, 0, cutoff, 1, hostCutoff } },
        { { 0 }, { 1 }, { 2, 3 } },
    };

    CHECK(checkEverySchedule(scenario) > 10000);
}

TEST_CASE("Every interleaving of two publishes with a host edit fits one serial order")
{
    const Scenario scenario {
        { { true, 1 }, { true, 2 }, { false, 0, gain, 1, hostSetting[1] } },
        { { 0, 1 }, { 2 } },
    };

    CHECK(checkEverySchedule(scenario) > 0);
}

TEST_CASE("Every interleaving of two host writers with two reads spanning them fits one serial order")
{
    // Thread 1 writes gain and thread 2 writes cutoff. One slow read pauses after loading gain,
    // the other after loading the parameter before gain, so each can load gain on either side of
    // its write; quick reads fall between every step.
    const Scenario scenario {
        { { false, 0, gain, 1, hostSetting[1] }, { false, 0, cutoff, 1, hostCutoff } },
        { { 0 }, { 1 } },
        { gain, gain - 1 },
    };

    int acceptedSlowReads = 0;
    CHECK(checkEverySchedule(scenario, &acceptedSlowReads) > 1000);
    CHECK(acceptedSlowReads > 0);
}

TEST_CASE("Every interleaving of a publish and a host write with a read spanning them fits one serial order")
{
    const Scenario scenario {
        { { true, 1 }, { false, 0, gain, 1, hostSetting[1] } },
        { { 0 }, { 1 } },
        { gain },
    };

    int acceptedSlowReads = 0;
    CHECK(checkEverySchedule(scenario, &acceptedSlowReads) > 100);
    CHECK(acceptedSlowReads > 0);
}

TEST_CASE("Two reads overlapping two pending host writes never both accept")
{
    // A slow read loads gain, then pauses. A writes gain and has not finished; a quick read runs;
    // B, begun after the quick read ended, writes cutoff and has not finished; the slow read loads
    // cutoff. The quick read could only have shown A, and the slow one only B: no single order
    // explains both, so neither may succeed.
    PatchState state(patchNumber(0));
    Steps::HostWrite writeA;
    Steps::HostWrite writeB;
    auto quick = patchNumber(2);
    auto slow = patchNumber(2);
    bool quickAccepted = true;

    const bool slowAccepted = Steps::readPatch(state, slow, [&](std::size_t index) {
        if (index != gain)
            return;

        writeA = Steps::beginHostWrite(state, gain, hostSetting[1]);
        REQUIRE(Steps::attemptHostWrite(state, writeA));
        quickAccepted = state.tryRead(quick);
        writeB = Steps::beginHostWrite(state, cutoff, hostCutoff);
        REQUIRE(Steps::attemptHostWrite(state, writeB));
    });

    CHECK_FALSE(quickAccepted);
    CHECK_FALSE(slowAccepted);
    CHECK(quick == patchNumber(2));
    CHECK(slow == patchNumber(2));

    // The writes themselves are kept; once both finish, reads succeed and show both.
    CHECK(state.hostValue(gain) == hostSetting[1]);
    CHECK(state.hostValue(cutoff) == hostCutoff);
    Steps::finishHostWrite(state);
    Steps::finishHostWrite(state);
    CHECK(classify(state.snapshot()) == Observed { 0, 1, 1 });
}

TEST_CASE("A host write delayed across a publish never hides a newer completed write")
{
    // A reads its parameter and stamps the current patch, then stalls. A publish completes, and
    // another thread's write B completes. While A is in progress no patch read succeeds; the
    // host's view of the parameter shows B until A takes effect after it.
    PatchState state(patchNumber(0));
    auto delayed = Steps::beginHostWrite(state, gain, hostSetting[1]);

    state.publish(patchNumber(1));
    state.setHostValue(gain, hostSetting[2]);

    auto read = patchNumber(2);
    CHECK_FALSE(state.tryRead(read));
    CHECK(read == patchNumber(2));
    CHECK(state.hostValue(gain) == hostSetting[2]);

    CHECK_FALSE(Steps::attemptHostWrite(state, delayed));
    CHECK(state.hostValue(gain) == hostSetting[2]);

    CHECK(Steps::attemptHostWrite(state, delayed));
    Steps::finishHostWrite(state);
    CHECK(classify(state.snapshot()) == Observed { 1, 1, 0 });
}

TEST_CASE("A host write that reads its parameter before a publish and the patch after it lands after the publish")
{
    // A whole publish completes between the write's two loads. The word the write read has since
    // been moved on, so its first attempt fails and its second lands against the new patch.
    PatchState state(patchNumber(0));
    auto write = Steps::beginHostWrite(state, gain, hostSetting[1]);
    Steps::prepareHostWrite(state, write, [&] { state.publish(patchNumber(1)); });

    CHECK_FALSE(Steps::attemptHostWrite(state, write));
    CHECK(Steps::attemptHostWrite(state, write));
    Steps::finishHostWrite(state);

    CHECK(classify(state.snapshot()) == Observed { 1, 1, 0 });
    CHECK(state.hostValue(gain) == hostSetting[1]);
}

TEST_CASE("Host edits made while a publish is in progress are replaced by it and reported")
{
    PatchState state(patchNumber(0));
    Steps::Publication publication;
    Steps::beginPublish(state, publication, patchNumber(1));

    state.setHostValue(gain, hostSetting[1]);
    state.setHostValue(cutoff, hostCutoff);

    contracts::InstrumentPatch read;
    CHECK_FALSE(state.tryRead(read));

    Steps::switchGeneration(state, publication);
    const auto changed = Steps::finishPublish(state, publication);

    REQUIRE(state.tryRead(read));
    CHECK(classify(read) == Observed { 1, 0, 0 });
    CHECK(changed.test(gain));
    CHECK(changed.test(cutoff));
}

TEST_CASE("A host write that keeps losing to concurrent writes stops after a fixed number of attempts")
{
    // Before each attempt another write of the same parameter completes, so every attempt finds
    // its parameter changed. The write gives up, ordered before the last of them, and still
    // finishes, so reads succeed again afterwards.
    PatchState state(patchNumber(0));
    int attempts = 0;

    Steps::writeHostValue(state, gain, hostSetting[1], [&](int attempt) {
        CHECK(attempt == attempts);
        ++attempts;
        state.setHostValue(gain, attempt % 2 == 0 ? hostSetting[2] : 0.5f);
    });

    CHECK(attempts == PatchState::maximumHostWriteAttempts);
    CHECK(state.hostValue(gain) == ((PatchState::maximumHostWriteAttempts - 1) % 2 == 0 ? hostSetting[2] : 0.5f));

    contracts::InstrumentPatch read;
    CHECK(state.tryRead(read));

    // Without interference the first attempt succeeds.
    attempts = 0;
    Steps::writeHostValue(state, gain, hostSetting[1], [&](int) { ++attempts; });
    CHECK(attempts == 1);
    CHECK(state.hostValue(gain) == hostSetting[1]);
}

TEST_CASE("The host value read is bounded and reports one side of a publish that interrupts it")
{
    auto louder = patchNumber(1);
    louder.gainDb = -6.0;

    // A publish switching patches inside the read: the read is repeated once, against the new
    // patch.
    {
        PatchState state(patchNumber(0));
        Steps::Publication publication;
        Steps::beginPublish(state, publication, louder);
        int interruptions = 0;

        const float value = Steps::readHostValue(state, gain, [&] {
            ++interruptions;
            Steps::switchGeneration(state, publication);
        });

        CHECK(interruptions == 1);
        CHECK(value == normalise(gain, -6.0));
        Steps::finishPublish(state, publication);
    }

    // Two publishes completing inside the read: the read still ends after one repetition and
    // reports a value from one side of the last publish.
    {
        PatchState state(patchNumber(0));
        auto quieter = patchNumber(2);
        quieter.gainDb = -30.0;
        int interruptions = 0;

        const float value = Steps::readHostValue(state, gain, [&] {
            ++interruptions;
            state.publish(louder);
            state.publish(quieter);
        });

        CHECK(interruptions == 1);
        CHECK((value == normalise(gain, -6.0) || value == normalise(gain, -30.0)));
    }
}

TEST_CASE("A patch read that a write or publish overlaps reports failure and changes nothing")
{
    PatchState state(patchNumber(0));
    auto untouched = patchNumber(2);
    const auto atLastLoad = [](auto action) {
        return [action](std::size_t index) mutable {
            if (index == parameterCount - 1)
                action();
        };
    };

    // A host write completing during the read.
    CHECK_FALSE(Steps::readPatch(state, untouched, atLastLoad([&] { state.setHostValue(gain, hostSetting[1]); })));
    CHECK(untouched == patchNumber(2));

    // A host write begun during the read and still in progress at its end, or begun before it.
    auto pending = Steps::beginHostWrite(state, cutoff, hostCutoff);
    CHECK_FALSE(Steps::readPatch(state, untouched, [](std::size_t) {}));
    Steps::finishHostWrite(state);
    CHECK_FALSE(Steps::readPatch(state, untouched, atLastLoad([&] { pending = Steps::beginHostWrite(state, cutoff, hostCutoff); })));
    Steps::finishHostWrite(state);
    CHECK(untouched == patchNumber(2));

    // A publish begun during the read.
    Steps::Publication publication;
    CHECK_FALSE(Steps::readPatch(state, untouched, atLastLoad([&] { Steps::beginPublish(state, publication, patchNumber(1)); })));
    CHECK(untouched == patchNumber(2));
    Steps::switchGeneration(state, publication);
    Steps::finishPublish(state, publication);

    // The host write came before the publish, which replaced it.
    contracts::InstrumentPatch read;
    CHECK(Steps::readPatch(state, read, [](std::size_t) {}));
    CHECK(classify(read) == Observed { 1, 0, 0 });
}

TEST_CASE("Reporting a value to the host never overwrites a host write")
{
    // The processor reports a parameter by reading it and passing the value on; it never writes
    // the value back. Replay a host write landing between that read and the report.
    PatchState state(patchNumber(0));
    state.publish(patchNumber(1));

    const float reported = state.hostValue(gain);
    state.setHostValue(gain, hostSetting[2]);
    CHECK(reported == normalise(gain, -12.0));

    CHECK(state.hostValue(gain) == hostSetting[2]);
    CHECK(classify(state.snapshot()) == Observed { 1, 2, 0 });
}

TEST_CASE("Concurrent host edits and publishes are always read in one order")
{
    // A host thread writes gain then cutoff, pair after pair; a publisher replaces the patch
    // with the same gain and cutoff over and over. A read may show a host gain from pair j only
    // with a cutoff from pair j or j - 1, or with the publish's cutoff while pair j's cutoff is
    // still to be written; and never a pair older than one completed before the read began.
    // Reads succeed only between writes, so the test does not depend on how many do; a read after
    // both threads stop must succeed and show the last pair or a later publish.
    constexpr int pairs = 500000;
    const auto hostSettingOf = [](int pair) { return static_cast<float>(pair) / 1048576.0f; };
    const auto pairOf = [](std::size_t index, double value) {
        return static_cast<int>(std::lround(static_cast<double>(normalise(index, value)) * 1048576.0));
    };

    PatchState state(patchNumber(0));
    std::atomic<int> completedPairs { 0 };
    std::atomic<int> publishes { 0 };
    std::atomic<bool> running { true };

    std::thread host([&] {
        while (publishes.load() == 0)
            std::this_thread::yield();

        for (int pair = 1; pair <= pairs; ++pair)
        {
            state.setHostValue(gain, hostSettingOf(pair));
            state.setHostValue(cutoff, hostSettingOf(pair));
            completedPairs.store(pair);
        }

        running = false;
    });

    std::thread publisher([&] {
        while (running)
        {
            state.publish(patchNumber(0));
            ++publishes;
        }
    });

    int violations = 0;

    const auto check = [&](const contracts::InstrumentPatch& read, int completed) {
        const bool gainFromPublish = read.gainDb == -12.0;
        const bool cutoffFromPublish = read.cutoffHz == 8000.0;
        const int gainPair = gainFromPublish ? 0 : pairOf(gain, read.gainDb);
        const int cutoffPair = cutoffFromPublish ? 0 : pairOf(cutoff, read.cutoffHz);

        if (! gainFromPublish && ! cutoffFromPublish && (gainPair < cutoffPair || gainPair > cutoffPair + 1))
            ++violations;

        // Pair j's cutoff was written before this read began, yet a publish replaced the cutoff
        // and not the gain written before it.
        if (! gainFromPublish && cutoffFromPublish && gainPair <= completed)
            ++violations;

        // A completed pair can only be replaced by a later pair or a publish.
        if ((! gainFromPublish && gainPair < completed) || (! cutoffFromPublish && cutoffPair < completed))
            ++violations;
    };

    while (running)
    {
        const int completed = completedPairs.load();
        contracts::InstrumentPatch read;

        if (state.tryRead(read))
            check(read, completed);
    }

    host.join();
    publisher.join();

    contracts::InstrumentPatch last;
    REQUIRE(state.tryRead(last));
    check(last, pairs);

    CHECK(publishes > 0);
    CHECK(violations == 0);
}
