#include <composer/project/ProjectSession.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>
#include <variant>
#include <vector>

// Compile this source in its own executable. These replacement allocators must
// not be linked into ordinary tests or combined with other allocator overrides.
namespace
{
struct AllocationProbe
{
    std::size_t selectedBytes = 0;
    std::size_t rejectedBytes = 0;
    unsigned matchingCalls = 0;
    unsigned failures = 0;
    bool armed = false;
};

thread_local AllocationProbe allocationProbe;

class OneShotAllocationFailure
{
public:
    explicit OneShotAllocationFailure(std::size_t bytes)
    {
        allocationProbe = {bytes, 0, 0, 0, true};
    }

    ~OneShotAllocationFailure() { allocationProbe = {}; }
    OneShotAllocationFailure(const OneShotAllocationFailure&) = delete;
    OneShotAllocationFailure& operator=(const OneShotAllocationFailure&) = delete;
};
}

void* operator new(std::size_t bytes)
{
    if (allocationProbe.selectedBytes != 0 && bytes == allocationProbe.selectedBytes)
    {
        ++allocationProbe.matchingCalls;
        if (allocationProbe.armed)
        {
            allocationProbe.armed = false;
            allocationProbe.rejectedBytes = bytes;
            ++allocationProbe.failures;
            throw std::bad_alloc();
        }
    }
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace composer::project;
using namespace composer::contracts;

namespace
{
template <class T>
T take(SessionResult<T> result)
{
    REQUIRE(std::holds_alternative<T>(result));
    return std::move(std::get<T>(result));
}

struct SessionSnapshot
{
    ProjectDocument document;
    ProjectPatchSnapshot patch;
    std::vector<InstrumentPatch> undo;
    std::vector<InstrumentPatch> redo;
    bool dirty;
    bool recording;
};

SessionSnapshot snapshot(const ProjectSession& session)
{
    return {session.document(), session.patchSnapshot(),
            {session.undoHistory().begin(), session.undoHistory().end()},
            {session.redoHistory().begin(), session.redoHistory().end()},
            session.isDirty(), session.isRecording()};
}

void unchanged(const ProjectSession& session, const SessionSnapshot& before)
{
    CHECK(session.document() == before.document);
    CHECK(session.patchSnapshot().patch == before.patch.patch);
    CHECK(session.patchSnapshot().projectInstanceId == before.patch.projectInstanceId);
    CHECK(session.patchSnapshot().revision == before.patch.revision);
    CHECK(session.context().projectInstanceId == before.patch.projectInstanceId);
    CHECK(session.context().revision == before.patch.revision);
    CHECK(std::ranges::equal(session.undoHistory(), before.undo));
    CHECK(std::ranges::equal(session.redoHistory(), before.redo));
    CHECK(session.isDirty() == before.dirty);
    CHECK(session.isRecording() == before.recording);
}
}

TEST_CASE("Session allocation guard rejects exactly one selected allocation", "[session][allocation]")
{
    bool rejected = false;
    void* following = nullptr;
    AllocationProbe observed;
    {
        const OneShotAllocationFailure failure(73);
        try
        {
            // Direct calls prevent a new-expression allocation from being elided.
            void* unexpected = ::operator new(73);
            ::operator delete(unexpected);
        }
        catch (const std::bad_alloc&) { rejected = true; }
        following = ::operator new(73);
        observed = allocationProbe;
    }
    const bool followingAllocated = following != nullptr;
    ::operator delete(following);
    REQUIRE(rejected);
    REQUIRE(followingAllocated);
    CHECK(observed.matchingCalls == 2);
    CHECK(observed.failures == 1);
    CHECK(observed.rejectedBytes == 73);
    CHECK_FALSE(observed.armed);
    CHECK_FALSE(allocationProbe.armed);
}

TEST_CASE("Recording copy allocation failure preserves the session and permits retry", "[session][allocation]")
{
    auto session = take(ProjectSession::create("allocation-session"));
    auto patch = session.document().patch;
    patch.gainDb = -18;
    REQUIRE(take(session.apply({"allocation-session", 0, patch})) == EditOutcome::applied);
    patch.gainDb = -24;
    REQUIRE(take(session.apply({"allocation-session", 1, patch})) == EditOutcome::applied);
    REQUIRE(take(session.undoPatch()) == EditOutcome::applied);
    REQUIRE_FALSE(session.undoHistory().empty());
    REQUIRE_FALSE(session.redoHistory().empty());
    const auto ticket = take(session.beginRecording());
    const auto ticketBefore = *ticket;
    const auto before = snapshot(session);

    ProjectDocument completed;
    completed.patch = session.document().patch;
    completed.durationSeconds = 8.0;
    constexpr std::size_t eventCount = 32;
    completed.events.reserve(eventCount);
    for (std::size_t index = 0; index < eventCount; ++index)
    {
        const auto channel = static_cast<std::uint8_t>((index / 2) % 3);
        const auto status = static_cast<std::uint8_t>((index % 2 == 0 ? 0x90 : 0x80) | channel);
        completed.events.push_back({static_cast<double>(index) * 0.25,
                                    {status, static_cast<std::uint8_t>(48 + index / 2), 67}});
    }
    REQUIRE_FALSE(validateProject(completed).has_value());
    const auto completedBefore = completed;

    // Select the contiguous MidiEvent copy allocation, not checked-STL proxies,
    // context strings or per-event MIDI storage. This valid 32-event fixture has
    // short field paths and three-byte messages. Staying below 4096 also avoids
    // the pinned MSVC allocator's large-allocation alignment adjustment.
    constexpr auto eventStorageBytes = eventCount * sizeof(MidiEvent);
    static_assert(eventStorageBytes > 256 && eventStorageBytes < 4096);

    // Confirm the selected size reaches the document-copy preparation itself.
    // This calibration and all assertions are outside the commit failure window.
    bool copyRejected = false;
    AllocationProbe copyProbe;
    {
        const OneShotAllocationFailure failure(eventStorageBytes);
        try
        {
            const ProjectDocument copy = completed;
            (void) copy;
        }
        catch (const std::bad_alloc&) { copyRejected = true; }
        copyProbe = allocationProbe;
    }
    REQUIRE(copyRejected);
    REQUIRE(copyProbe.failures == 1);
    REQUIRE(copyProbe.rejectedBytes == eventStorageBytes);
    CHECK(completed == completedBefore);
    unchanged(session, before);

    SessionResult<EditOutcome> result = EditOutcome::noChange;
    AllocationProbe commitProbe;
    {
        const OneShotAllocationFailure failure(eventStorageBytes);
        result = session.commitRecording(*ticket, completed);
        commitProbe = allocationProbe;
    }

    REQUIRE(commitProbe.failures == 1);
    REQUIRE(commitProbe.rejectedBytes == eventStorageBytes);
    CHECK(commitProbe.matchingCalls == 1);
    CHECK_FALSE(commitProbe.armed);
    CHECK_FALSE(allocationProbe.armed);
    REQUIRE(std::holds_alternative<SessionError>(result));
    CHECK(std::get<SessionError>(result).code == SessionErrorCode::resourceLimit);
    unchanged(session, before);
    CHECK(completed == completedBefore);
    CHECK(ticket->attempt == ticketBefore.attempt);
    CHECK(ticket->context.projectInstanceId == ticketBefore.context.projectInstanceId);
    CHECK(ticket->context.revision == ticketBefore.context.revision);

    REQUIRE(take(session.commitRecording(*ticket, completed)) == EditOutcome::applied);
    CHECK(session.document() == completedBefore);
    CHECK(completed == completedBefore);
    CHECK(session.context().projectInstanceId == before.patch.projectInstanceId);
    CHECK(session.context().revision == before.patch.revision + 1);
    CHECK(session.patchSnapshot().patch == completedBefore.patch);
    CHECK(std::ranges::equal(session.undoHistory(), before.undo));
    CHECK(std::ranges::equal(session.redoHistory(), before.redo));
    CHECK(session.isDirty());
    CHECK_FALSE(session.isRecording());
    CHECK(ticket->attempt == ticketBefore.attempt);
    CHECK(ticket->context.projectInstanceId == ticketBefore.context.projectInstanceId);
    CHECK(ticket->context.revision == ticketBefore.context.revision);
}
