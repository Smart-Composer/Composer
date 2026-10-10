#pragma once

#include <composer_v1/instrument/synth/ParameterCurves.h>

#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace composer_v1::instrument::synth
{

/** The instrument's patch, shared by the editing thread, the host's parameter calls and the
    audio thread.

    It holds the last published patch, one exact double per parameter, and at most one host value
    per parameter on top of it. A host value equal to the published value's normalised setting
    keeps the published double exactly, so a published patch survives host echoes; any other host
    value stands for the value its normalised setting represents.

    Order: every publish and every host write takes effect at a single point within its call, in
    one order that every read agrees with. A publish replaces the whole patch at once, including
    every host value written before it. A host write replaces one value and never overwrites a
    value written after it. Host writes from one thread keep their order. tryRead and snapshot
    return only states from that order, including every write that completed before the read: a
    patch read succeeds only when no publish or host write was in progress at any point during it.

    Reporting: a host value that a publish replaces is listed among the parameters the publish
    changed, so the host can be told the new value, unless the publish sets that parameter to the
    same setting or a later host write has already replaced the publish's value. A parameter is
    listed only when the publish changes its setting or replaces a host value.

    Progress: host writes, tryRead and hostValue never wait and take a bounded number of steps.
    tryRead reports failure, leaving its output untouched, while a publish or host write is in
    progress; snapshot, which is not for the audio thread, tries again until one succeeds.
    A host write retries only when another write or a publish changes its parameter during the
    attempt, at most maximumHostWriteAttempts times; a write that loses every attempt is ordered
    immediately before the change that beat it, and has no effect. hostValue reads at most twice;
    while publishes keep completing during it, it reports the parameter from one side of the
    last of them. Publishes are serialised with a mutex and never run on the audio thread.
*/
class PatchState
{
public:
    static constexpr int maximumHostWriteAttempts = 8;

    explicit PatchState(const contracts::InstrumentPatch& initial = {}) noexcept;

    PatchState(const PatchState&) = delete;
    PatchState& operator=(const PatchState&) = delete;

    /** Replaces the patch with a validated one. Not for the audio thread. Returns the parameters
        whose host value the publish changed, including any host value it replaced. */
    std::bitset<parameterCount> publish(const contracts::InstrumentPatch& validated);

    /** The current patch. Not for the audio thread. */
    contracts::InstrumentPatch snapshot() const;

    /** Reads the current patch without waiting or allocating. Returns false, leaving out
        untouched, when a publish or host write is in progress at any point during the read. */
    bool tryRead(contracts::InstrumentPatch& out) const noexcept
    {
        return readPatch(out, [](std::size_t) {});
    }

    /** The normalised value the host sees for a parameter. Any thread, including the audio
        thread. */
    float hostValue(std::size_t index) const noexcept
    {
        return readHostValue(index, [] {});
    }

    /** Applies a normalised host value to one parameter. Any thread, including the audio thread.
        NaN is ignored, and out-of-range values clamp. */
    void setHostValue(std::size_t index, float normalised) noexcept
    {
        writeHostValue(index, normalised, [](int) {});
    }

private:
    // Deterministic interleaving tests drive the steps below one at a time.
    friend struct PatchStateSteps;

    struct Publication
    {
        std::uint32_t current = 0;
        std::uint32_t next = 0;
        std::uint32_t sequence = 0;
        std::array<float, parameterCount> before {};
        std::array<float, parameterCount> after {};
        std::bitset<parameterCount> replaced;
        std::array<float, parameterCount> replacedValues {};
    };

    struct HostWrite
    {
        std::size_t index = 0;
        float value = 0.0f;
        std::uint64_t expected = 0;
        std::uint64_t desired = 0;
    };

    // Publication, with publishMutex held: begin stages the next patch in the slot the current
    // generation does not use; switchGeneration makes it current; finish moves every parameter's
    // host word to the new generation, recording any host value written against the old one.
    void beginPublish(Publication& publication, const contracts::InstrumentPatch& validated) noexcept;
    void switchGeneration(const Publication& publication) noexcept;
    std::bitset<parameterCount> finishPublish(Publication& publication) noexcept;

    // A host write: begin registers the write as in progress and reads the parameter's word and
    // then the generation; each attempt installs the value, stamped with that generation, only if
    // the word is still the one read, and otherwise reads both again; finish counts the write as
    // completed and then ends its registration. Every begin needs exactly one finish.
    HostWrite beginHostWrite(std::size_t index, float normalised) noexcept;
    bool attemptHostWrite(HostWrite& write) noexcept;
    void finishHostWrite() noexcept;

    // Reads the word, then the generation. betweenLoads runs between the two loads, so tests can
    // place a publish there.
    template <typename BetweenLoads>
    void prepareHostWrite(HostWrite& write, BetweenLoads&& betweenLoads) const noexcept;
    static std::uint64_t hostWord(std::uint32_t generation, float value) noexcept;

    // The operations behind setHostValue, tryRead and hostValue. The write calls beforeAttempt
    // with the attempt's number before each attempt; interrupt runs inside a read, between loading
    // the state and checking it. Tests use them to place other operations there.
    template <typename BeforeAttempt>
    void writeHostValue(std::size_t index, float normalised, BeforeAttempt&& beforeAttempt) noexcept;

    template <typename AfterLoad>
    bool readPatch(contracts::InstrumentPatch& out, AfterLoad&& afterLoad) const noexcept;

    template <typename Interrupt>
    float readHostValue(std::size_t index, Interrupt&& interrupt) const noexcept;

    double currentValue(std::size_t index, std::uint64_t word, std::uint32_t generation, double published) const noexcept;
    float hostValueAt(std::size_t index, std::uint32_t generation) const noexcept;
    void storePatch(contracts::InstrumentPatch& out,
                    std::uint32_t generation,
                    const std::array<double, parameterCount>& values,
                    const std::array<std::uint64_t, parameterCount>& words) const noexcept;

    mutable std::mutex publishMutex;

    // Odd while a publish is between beginPublish and finishPublish.
    std::atomic<std::uint32_t> sequence { 0 };

    // The current patch's generation. The published values alternate between two slots by its
    // parity, so a publish stages the next patch without touching the current one.
    std::atomic<std::uint32_t> generation { 1 };
    std::array<std::array<std::atomic<double>, parameterCount>, 2> published {};

    // Per parameter: the generation the host value was written against, and the host value's
    // float bits, or no value. After every publish each word carries the current generation.
    std::array<std::atomic<std::uint64_t>, parameterCount> hostWords {};

    // Host writes in progress, and host writes completed. A patch read succeeds only if none was
    // in progress when it began or when it ended, and none completed in between, so no host write
    // overlapped it. The completed count is 64 bits wide so that it cannot wrap during a read.
    std::atomic<std::uint32_t> activeHostWrites { 0 };
    std::atomic<std::uint64_t> hostWrites { 0 };

    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

template <typename BetweenLoads>
void PatchState::prepareHostWrite(HostWrite& write, BetweenLoads&& betweenLoads) const noexcept
{
    // The word first, then the generation: if the word already shows a publish's pass, the
    // generation read afterwards is that publish's, so a write never stamps a replaced patch
    // over a word that publish has already moved on.
    write.expected = hostWords[write.index].load();
    betweenLoads();
    write.desired = hostWord(generation.load(), write.value);
}

template <typename BeforeAttempt>
void PatchState::writeHostValue(std::size_t index, float normalised, BeforeAttempt&& beforeAttempt) noexcept
{
    if (std::isnan(normalised))
        return;

    auto write = beginHostWrite(index, normalised);

    for (int attempt = 0; attempt < maximumHostWriteAttempts; ++attempt)
    {
        beforeAttempt(attempt);

        if (attemptHostWrite(write))
            break;
    }

    finishHostWrite();
}

template <typename AfterLoad>
bool PatchState::readPatch(contracts::InstrumentPatch& out, AfterLoad&& afterLoad) const noexcept
{
    // The completed count is read before the in-progress count at the start, and after it at the
    // end. A host write that overlaps the loads is then either still in progress at the end, or
    // has completed and changed the count.
    const auto start = sequence.load();
    const auto writes = hostWrites.load();

    if ((start & 1u) != 0 || activeHostWrites.load() != 0)
        return false;

    const auto current = generation.load();
    std::array<double, parameterCount> values {};
    std::array<std::uint64_t, parameterCount> words {};

    for (std::size_t index = 0; index < parameterCount; ++index)
    {
        values[index] = published[current & 1u][index].load();
        words[index] = hostWords[index].load();
        afterLoad(index);
    }

    if (sequence.load() != start || activeHostWrites.load() != 0 || hostWrites.load() != writes)
        return false;

    storePatch(out, current, values, words);
    return true;
}

template <typename Interrupt>
float PatchState::readHostValue(std::size_t index, Interrupt&& interrupt) const noexcept
{
    // A second read is needed only when a publish switched generation during the first; the
    // second is reported even if another publish switches during it.
    auto current = generation.load();
    float value = hostValueAt(index, current);
    interrupt();

    if (const auto now = generation.load(); now != current)
        value = hostValueAt(index, now);

    return value;
}

} // namespace composer_v1::instrument::synth
