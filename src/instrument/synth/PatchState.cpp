#include <composer/instrument/synth/PatchState.h>

#include <algorithm>
#include <bit>
#include <thread>

namespace composer::instrument::synth
{
namespace
{

// A host word: bits 63-32 are the generation the value was written against, bits 31-0 the
// value's float bits. Every operation on the words, the generation and the counters is
// sequentially consistent, so all threads agree on one order of publishes and host writes.

// A NaN, which no stored host value can be.
constexpr std::uint32_t noValue = 0xFFFFFFFFu;

std::uint64_t makeWord(std::uint32_t generation, std::uint32_t valueBits) noexcept
{
    return (static_cast<std::uint64_t>(generation) << 32) | valueBits;
}

std::uint64_t makeWord(std::uint32_t generation, float value) noexcept
{
    return makeWord(generation, std::bit_cast<std::uint32_t>(value));
}

std::uint32_t generationOf(std::uint64_t word) noexcept
{
    return static_cast<std::uint32_t>(word >> 32);
}

bool hasValue(std::uint64_t word) noexcept
{
    return static_cast<std::uint32_t>(word) != noValue;
}

float valueOf(std::uint64_t word) noexcept
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(word));
}

} // namespace

PatchState::PatchState(const contracts::InstrumentPatch& initial) noexcept
{
    const auto current = generation.load();

    for (std::size_t index = 0; index < parameterCount; ++index)
    {
        published[0][index].store(fieldValue(initial, index));
        published[1][index].store(fieldValue(initial, index));
        hostWords[index].store(makeWord(current, noValue));
    }
}

std::bitset<parameterCount> PatchState::publish(const contracts::InstrumentPatch& validated)
{
    const std::scoped_lock lock(publishMutex);

    Publication publication;
    beginPublish(publication, validated);
    switchGeneration(publication);
    return finishPublish(publication);
}

void PatchState::beginPublish(Publication& publication, const contracts::InstrumentPatch& validated) noexcept
{
    publication.current = generation.load();
    publication.next = publication.current + 1;

    for (std::size_t index = 0; index < parameterCount; ++index)
    {
        publication.before[index] = hostValueAt(index, publication.current);
        publication.after[index] = normalise(index, fieldValue(validated, index));
    }

    publication.sequence = sequence.load();
    sequence.store(publication.sequence + 1);

    auto& staged = published[publication.next & 1u];

    for (std::size_t index = 0; index < parameterCount; ++index)
        staged[index].store(fieldValue(validated, index));
}

void PatchState::switchGeneration(const Publication& publication) noexcept
{
    generation.store(publication.next);
}

std::bitset<parameterCount> PatchState::finishPublish(Publication& publication) noexcept
{
    // Move every word to the new generation. A host value still stamped with the old one was
    // written before the switch: the publish replaces it and reports it. A writer whose attempt
    // overlaps this pass finds its word changed and tries again against the new patch.
    for (std::size_t index = 0; index < parameterCount; ++index)
    {
        auto word = hostWords[index].load();

        while (generationOf(word) != publication.next)
        {
            if (hostWords[index].compare_exchange_weak(word, makeWord(publication.next, noValue)))
            {
                if (hasValue(word))
                {
                    publication.replaced.set(index);
                    publication.replacedValues[index] = valueOf(word);
                }

                break;
            }
        }
    }

    sequence.store(publication.sequence + 2);

    std::bitset<parameterCount> changed;

    for (std::size_t index = 0; index < parameterCount; ++index)
    {
        const float after = publication.after[index];
        changed.set(index, publication.before[index] != after
                               || (publication.replaced.test(index) && publication.replacedValues[index] != after));
    }

    return changed;
}

contracts::InstrumentPatch PatchState::snapshot() const
{
    const std::scoped_lock lock(publishMutex);

    // With publishes excluded, only a host write overlapping the read makes it try again.
    contracts::InstrumentPatch patch;

    while (! tryRead(patch))
        std::this_thread::yield();

    return patch;
}

void PatchState::storePatch(contracts::InstrumentPatch& out,
                            std::uint32_t current,
                            const std::array<double, parameterCount>& values,
                            const std::array<std::uint64_t, parameterCount>& words) const noexcept
{
    for (std::size_t index = 0; index < parameterCount; ++index)
        setFieldValue(out, index, currentValue(index, words[index], current, values[index]));
}

float PatchState::hostValueAt(std::size_t index, std::uint32_t current) const noexcept
{
    const auto word = hostWords[index].load();
    return normalise(index, currentValue(index, word, current, published[current & 1u][index].load()));
}

double PatchState::currentValue(std::size_t index, std::uint64_t word, std::uint32_t current, double value) const noexcept
{
    if (! hasValue(word) || generationOf(word) != current)
        return value;

    // A host value that repeats the published value's setting is an echo: keep the exact double.
    const float hostSetting = valueOf(word);
    return hostSetting == normalise(index, value) ? value : denormalise(index, hostSetting);
}

PatchState::HostWrite PatchState::beginHostWrite(std::size_t index, float normalised) noexcept
{
    // Registered before any load or store, so a patch read that overlaps the write sees it.
    activeHostWrites.fetch_add(1);

    normalised = std::clamp(normalised, 0.0f, 1.0f);

    if (normalised == 0.0f)
        normalised = 0.0f;

    HostWrite write;
    write.index = index;
    write.value = normalised;
    prepareHostWrite(write, [] {});
    return write;
}

std::uint64_t PatchState::hostWord(std::uint32_t current, float value) noexcept
{
    return makeWord(current, value);
}

bool PatchState::attemptHostWrite(HostWrite& write) noexcept
{
    if (hostWords[write.index].compare_exchange_strong(write.expected, write.desired))
        return true;

    prepareHostWrite(write, [] {});
    return false;
}

void PatchState::finishHostWrite() noexcept
{
    // Counted as completed before its registration ends, so a read never misses it in both.
    hostWrites.fetch_add(1);
    activeHostWrites.fetch_sub(1);
}

} // namespace composer::instrument::synth
