#include "ApplicationProject.h"
#include "InstrumentAdapter.h"

#include <composer/engine/EngineSetup.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace
{
namespace app = composer::app;
namespace contracts = composer::contracts;
namespace project = composer::project;
constexpr double initialRate = 48000.0;
constexpr int blockSize = 128;

struct ScratchDirectory
{
    const juce::File parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const juce::File directory = parent.getChildFile("composer-application-" + juce::Uuid().toString());
    ScratchDirectory()
    {
        REQUIRE(directory.isAChildOf(parent));
        REQUIRE_FALSE(directory.exists());
        REQUIRE(directory.createDirectory().wasOk());
    }
    ~ScratchDirectory()
    {
        if (directory.isAChildOf(parent) && directory.getFileName().startsWith("composer-application-"))
            directory.deleteRecursively();
    }
};

// Observes the running engine immediately after the instrument. It does not
// schedule messages or call the instrument; all storage precedes graph startup.
class GraphObserver final : public tracktion::Plugin
{
public:
    static constexpr const char* xmlTypeName = "composer_test_project_observer";
    static const char* getPluginName() { return "Project Playback Observer"; }
    explicit GraphObserver(tracktion::PluginCreationInfo info) : Plugin(info) {}
    ~GraphObserver() override { notifyListenersOfDeletion(); }
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override { return true; }
    bool takesAudioInput() override { return true; }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    void initialise(const tracktion::PluginInitialisationInfo& info) override { rate = info.sampleRate; }
    void deinitialise() override { ++deinitialisations; }
    void applyToBuffer(const tracktion::PluginRenderContext& context) override
    {
        if (!context.isPlaying) return;
        ++contexts;
        if (rate != expectedRate || context.destBuffer == nullptr
            || context.destBuffer->getNumChannels() < 2 || context.bufferStartSample < 0
            || context.bufferNumSamples < 0 || context.bufferStartSample > context.destBuffer->getNumSamples()
            || context.bufferNumSamples > context.destBuffer->getNumSamples() - context.bufferStartSample)
        {
            ++invalidContexts;
            return;
        }
        const auto first = std::llround(context.editTime.getStart().inSeconds() * rate);
        for (int offset = 0; offset < context.bufferNumSamples; ++offset)
        {
            const auto sample = first + offset;
            if (sample < 0 || static_cast<std::size_t>(sample) >= audio.size()) continue;
            const auto index = static_cast<std::size_t>(sample);
            if (coverage[index] != 0) ++duplicates;
            coverage[index] = 1;
            for (int channel = 0; channel < 2; ++channel)
                audio[index][static_cast<std::size_t>(channel)] = context.destBuffer->getSample(channel, context.bufferStartSample + offset);
        }
    }

    std::vector<std::array<float, 2>> audio;
    std::vector<std::uint8_t> coverage;
    double expectedRate = 0.0;
    double rate = 0.0;
    std::size_t contexts = 0, invalidContexts = 0, duplicates = 0;
    int deinitialisations = 0;
};

struct Fixture
{
    ScratchDirectory scratch;
    std::unique_ptr<tracktion::Engine> engine =
        composer::engine::createHeadlessEngine("ComposerApplicationTests", scratch.directory);
    std::unique_ptr<app::ApplicationProject> owner;

    Fixture()
    {
        engine->getPluginManager().createBuiltInType<GraphObserver>();
        auto& device = engine->getDeviceManager().getHostedAudioDeviceInterface();
        tracktion::HostedAudioDeviceInterface::Parameters parameters;
        parameters.sampleRate = initialRate;
        parameters.blockSize = blockSize;
        parameters.useMidiDevices = false;
        parameters.inputChannels = 0;
        parameters.outputChannels = 2;
        device.initialise(parameters);
        device.prepareToPlay(initialRate, blockSize);
        engine->getDeviceManager().dispatchPendingUpdates();
        owner = std::make_unique<app::ApplicationProject>(*engine, initialRate);
    }
};

contracts::ProjectCommand commandFor(const app::ApplicationProject& owner, const contracts::InstrumentPatch& patch)
{
    const auto context = owner.session().context();
    return {context.projectInstanceId, context.revision, patch};
}

template <class T>
void requireApplied(const app::ApplicationResult<T>& result)
{
    REQUIRE(std::holds_alternative<contracts::EditOutcome>(result));
    REQUIRE(std::get<contracts::EditOutcome>(result) == contracts::EditOutcome::applied);
}

void requireOk(const app::ApplicationStatus& result)
{
    REQUIRE(std::holds_alternative<std::monostate>(result));
}

std::unique_ptr<project::RecordingTicket> beginRecording(app::ApplicationProject& owner)
{
    auto result = owner.beginRecording();
    REQUIRE(std::holds_alternative<std::unique_ptr<project::RecordingTicket>>(result));
    auto ticket = std::move(std::get<std::unique_ptr<project::RecordingTicket>>(result));
    REQUIRE(ticket != nullptr);
    return ticket;
}

struct Snapshot
{
    const project::ProjectDocument document;
    const project::ProjectContext context;
    const bool dirty;
    const tracktion::Edit* graph;
    explicit Snapshot(const app::ApplicationProject& owner)
        : document(owner.session().document()), context(owner.session().context()),
          dirty(owner.session().isDirty()), graph(&owner.playbackEdit()) {}
    void requireUnchanged(const app::ApplicationProject& owner) const
    {
        REQUIRE(owner.session().document() == document);
        REQUIRE(owner.session().context().projectInstanceId == context.projectInstanceId);
        REQUIRE(owner.session().context().revision == context.revision);
        REQUIRE(owner.session().isDirty() == dirty);
        REQUIRE(&owner.playbackEdit() == graph);
        REQUIRE(owner.playbackPatch() == document.patch);
    }
};

project::ProjectDocument shortDocument()
{
    project::ProjectDocument document;
    document.durationSeconds = 0.5;
    document.events = {{0.05, {0x90, 60, 100}}, {0.4, {0x80, 60, 73}},
                       {0.4, {0xb0, 1, 37}}, {0.5, {0xb1, 11, 87}}};
    return document;
}

project::CapturedMidi capture(const project::ProjectDocument& document)
{
    project::MidiCaptureBuffer buffer(std::max(std::size_t{1}, document.events.size()));
    REQUIRE(buffer.start() == project::CaptureStartResult::started);
    bool accepted = true;
    for (const auto& event : document.events)
        accepted &= buffer.submit(event.timeSeconds, event.bytes) == project::CaptureSubmitResult::accepted;
    REQUIRE(accepted);
    auto result = buffer.finish();
    REQUIRE(result.has_value());
    REQUIRE(result->isComplete());
    REQUIRE(result->events == document.events);
    return std::move(*result);
}

void installTake(app::ApplicationProject& owner, const project::ProjectDocument& document)
{
    if (document.patch != owner.session().document().patch)
        requireApplied(owner.apply(commandFor(owner, document.patch)));
    const auto ticket = beginRecording(owner);
    requireApplied(owner.commitRecording(*ticket, capture(document), document.durationSeconds));
    REQUIRE(owner.session().document() == document);
    REQUIRE(owner.playbackPatch() == document.patch);
}

struct Rendered
{
    std::vector<std::array<float, 2>> audio;
    double peak = 0.0;
};

Rendered render(Fixture& fixture, double seconds)
{
    auto& owner = *fixture.owner;
    const auto patch = owner.session().document().patch;
    auto observerPlugin = owner.playbackEdit().getPluginCache().createNewPlugin(GraphObserver::xmlTypeName, {});
    auto* observer = dynamic_cast<GraphObserver*>(observerPlugin.get());
    REQUIRE(observer != nullptr);
    observer->expectedRate = owner.playbackSampleRate();
    const auto sampleCount = static_cast<std::size_t>(std::llround(seconds * observer->expectedRate));
    observer->audio.resize(sampleCount);
    observer->coverage.resize(sampleCount, 0);
    owner.playbackTrack().pluginList.insertPlugin(observerPlugin, 1, nullptr);
    auto* adapter = owner.playbackTrack().pluginList.findFirstPluginOfType<app::InstrumentAdapter>();
    REQUIRE(adapter != nullptr);
    auto& devices = fixture.engine->getDeviceManager();
    auto& hosted = devices.getHostedAudioDeviceInterface();
    auto& transport = owner.playbackEdit().getTransport();
    transport.ensureContextAllocated(true);
    devices.dispatchPendingUpdates();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    requireOk(owner.playFromStart());
    REQUIRE(transport.isPlaying());
    juce::AudioBuffer<float> output(2, blockSize);
    juce::MidiBuffer midi;
    const auto callbackLimit = sampleCount / blockSize + 2048;
    bool finished = false, finite = true, contextPresent = true;
    double hostedPeak = 0.0;
    for (std::size_t index = 0; index < callbackLimit; ++index)
    {
        const auto* context = transport.getCurrentPlaybackContext();
        if (context == nullptr) { contextPresent = false; break; }
        if (context->globalStreamTimeToEditTime(devices.getCurrentStreamTime()).inSeconds() >= seconds + 0.02)
        {
            finished = true;
            break;
        }
        output.clear();
        midi.clear();
        hosted.processBlock(output, midi);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = output.getSample(channel, sample);
                finite &= std::isfinite(value);
                hostedPeak = std::max(hostedPeak, static_cast<double>(std::abs(value)));
            }
        if (index % 256 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    REQUIRE(finished);
    REQUIRE(contextPresent);
    REQUIRE(finite);
    REQUIRE(hostedPeak > 0.001);
    REQUIRE(observer->contexts > 0);
    REQUIRE(observer->invalidContexts == 0);
    REQUIRE(observer->duplicates == 0);
    REQUIRE(static_cast<std::size_t>(std::count(observer->coverage.begin(), observer->coverage.end(), std::uint8_t{1})) == sampleCount);
    REQUIRE(adapter->consumeRenderFaults() == 0);
    owner.stop();
    REQUIRE_FALSE(transport.isPlaying());
    REQUIRE(transport.getCurrentPlaybackContext() == nullptr);
    REQUIRE(observer->deinitialisations > 0);
    REQUIRE(owner.playbackPatch() == patch);
    Rendered result{std::move(observer->audio)};
    for (const auto& sample : result.audio)
        for (const float value : sample)
        {
            finite &= std::isfinite(value);
            result.peak = std::max(result.peak, static_cast<double>(std::abs(value)));
        }
    REQUIRE(finite);
    REQUIRE(result.peak > 0.001);
    observer->removeFromParent();
    return result;
}

bool identicalAudio(const Rendered& left, const Rendered& right)
{
    return left.audio.size() == right.audio.size()
        && std::memcmp(left.audio.data(), right.audio.data(), left.audio.size() * sizeof(left.audio[0])) == 0;
}

project::ProjectDocument minuteDocument()
{
    project::ProjectDocument result;
    result.durationSeconds = 60.0;
    result.patch = {contracts::Waveform::square, -15.25, 0.017, 0.123456789,
                    0.625, 0.3333333333333333, 7654.321, 0.7071067811865476};
    for (int index = 0; index < 120; ++index)
    {
        const int onSample = 12000 + index * 23777 + (index * 37) % 97;
        const int offSample = onSample + 3317 + (index * 29) % 1200;
        const auto channel = static_cast<std::uint8_t>(index % 3);
        const auto pitch = static_cast<std::uint8_t>(36 + index % 48);
        const auto start = onSample / initialRate;
        result.events.push_back({start, {static_cast<std::uint8_t>(0x90 | channel), pitch,
                                       static_cast<std::uint8_t>(25 + index % 100)}});
        if (index % 10 == 0)
        {
            result.events.push_back({start, {static_cast<std::uint8_t>(0xb0 | channel), 7, 99}});
            result.events.push_back({start, {static_cast<std::uint8_t>(0xe0 | channel), 35, 72}});
            result.events.push_back({start, {static_cast<std::uint8_t>(0xd0 | channel), 47}});
            result.events.push_back({start, {static_cast<std::uint8_t>(0xa0 | channel), pitch, 52}});
            result.events.push_back({start, {static_cast<std::uint8_t>(0xc0 | channel), 13}});
        }
        result.events.push_back({offSample / initialRate,
            {static_cast<std::uint8_t>(0x80 | channel), pitch, static_cast<std::uint8_t>(1 + index % 100)}});
    }
    for (int value = 0; value < 24; ++value)
        result.events.push_back({3.5, {0xb0, 1, static_cast<std::uint8_t>(value)}});
    for (const std::uint8_t channel : {std::uint8_t{2}, std::uint8_t{0}, std::uint8_t{1}})
        result.events.push_back({582001.0 / initialRate, {static_cast<std::uint8_t>(0xb0 | channel), 11, 64}});
    result.events.push_back({59.8, {0x92, 69, 111}});
    result.events.push_back({60.0, {0x82, 69, 73}});
    result.events.push_back({60.0, {0xb2, 1, 29}});
    std::stable_sort(result.events.begin(), result.events.end(), [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
    REQUIRE_FALSE(project::validateProject(result));
    return result;
}
}

TEST_CASE("Application patch edits keep the document and running instrument coherent", "[app][application-project]")
{
    Fixture fixture;
    auto& owner = *fixture.owner;
    const auto document = shortDocument();
    installTake(owner, document);
    const auto original = render(fixture, 1.0);
    const auto stale = commandFor(owner, document.patch);
    auto quieter = document.patch;
    quieter.gainDb = -30.0;
    requireApplied(owner.apply(commandFor(owner, quieter)));
    REQUIRE(owner.session().document().events == document.events);
    REQUIRE(owner.session().document().durationSeconds == document.durationSeconds);
    REQUIRE(owner.playbackPatch() == quieter);
    const auto quiet = render(fixture, 1.0);
    REQUIRE(quiet.peak < original.peak * 0.2);
    REQUIRE_FALSE(identicalAudio(original, quiet));
    requireApplied(owner.undoPatch());
    REQUIRE(owner.session().document() == document);
    REQUIRE(identicalAudio(original, render(fixture, 1.0)));
    requireApplied(owner.redoPatch());
    REQUIRE(owner.playbackPatch() == quieter);
    REQUIRE(identicalAudio(quiet, render(fixture, 1.0)));
    const Snapshot snapshot(owner);
    const auto rejected = owner.apply(stale);
    REQUIRE(std::holds_alternative<contracts::ContractError>(rejected));
    REQUIRE(std::get<contracts::ContractError>(rejected).code == contracts::ErrorCode::staleRevision);
    snapshot.requireUnchanged(owner);
    auto invalid = commandFor(owner, quieter);
    invalid.patch.gainDb = 1.0;
    REQUIRE(std::holds_alternative<contracts::ContractError>(owner.apply(invalid)));
    snapshot.requireUnchanged(owner);
    const auto unchanged = owner.apply(commandFor(owner, quieter));
    REQUIRE(std::get<contracts::EditOutcome>(unchanged) == contracts::EditOutcome::noChange);
    snapshot.requireUnchanged(owner);
    REQUIRE(identicalAudio(quiet, render(fixture, 1.0)));
}

TEST_CASE("Application saves and reopens one minute of captured MIDI with identical graph audio", "[app][application-project]")
{
    ScratchDirectory files;
    const auto path = files.directory.getChildFile("minute.composer");
    const auto document = minuteDocument();
    Rendered original;
    std::string originalToken;
    {
        Fixture fixture;
        auto& owner = *fixture.owner;
        requireApplied(owner.apply(commandFor(owner, document.patch)));
        const auto beforeCapture = commandFor(owner, document.patch);
        const auto ticket = beginRecording(owner);
        requireApplied(owner.commitRecording(*ticket, capture(document), document.durationSeconds));
        originalToken = owner.session().context().projectInstanceId;
        REQUIRE(owner.session().context().revision == 2);
        REQUIRE(owner.session().isDirty());
        REQUIRE(owner.playbackConversion().clips.size() == 3);
        const auto obsolete = owner.apply(beforeCapture);
        REQUIRE(std::get<contracts::ContractError>(obsolete).code == contracts::ErrorCode::staleRevision);
        REQUIRE(owner.session().document() == document);
        original = render(fixture, 60.5);
        const auto saved = owner.save(path);
        REQUIRE(std::holds_alternative<project::SaveReceipt>(saved));
        REQUIRE_FALSE(owner.session().isDirty());
        REQUIRE(owner.session().document() == document);
        const auto loaded = project::loadProjectFile(path);
        REQUIRE(std::get<project::ProjectDocument>(loaded) == document);
    }
    {
        Fixture fixture;
        auto& owner = *fixture.owner;
        const auto emptyToken = owner.session().context().projectInstanceId;
        requireOk(owner.open(path));
        REQUIRE(owner.session().context().projectInstanceId != emptyToken);
        REQUIRE(owner.session().context().projectInstanceId != originalToken);
        REQUIRE(owner.session().context().revision == 0);
        REQUIRE(owner.session().undoHistory().empty());
        REQUIRE(owner.session().redoHistory().empty());
        REQUIRE_FALSE(owner.session().isDirty());
        REQUIRE(owner.session().document() == document);
        REQUIRE(owner.playbackPatch() == document.patch);
        REQUIRE(identicalAudio(original, render(fixture, 60.5)));
    }
    double tailPeak = 0.0, finalPeak = 0.0;
    for (std::size_t index = 60 * 48000; index < original.audio.size(); ++index)
        for (const auto value : original.audio[index])
        {
            tailPeak = std::max(tailPeak, static_cast<double>(std::abs(value)));
            if (index >= 60 * 48000 + 19200) finalPeak = std::max(finalPeak, static_cast<double>(std::abs(value)));
        }
    REQUIRE(tailPeak > 0.001);
    REQUIRE(finalPeak == 0.0);
}

TEST_CASE("Application retains incomplete takes for retry and freezes project edits", "[app][application-project]")
{
    Fixture fixture;
    auto& owner = *fixture.owner;
    const auto document = shortDocument();
    auto completed = capture(document);
    const Snapshot empty(owner);
    const auto ticket = beginRecording(owner);
    auto patch = document.patch;
    patch.gainDb = -18.0;
    REQUIRE(std::get<project::SessionError>(owner.apply(commandFor(owner, patch))).code == project::SessionErrorCode::recordingActive);
    REQUIRE(std::get<project::SessionError>(owner.undoPatch()).code == project::SessionErrorCode::recordingActive);
    REQUIRE(std::get<project::SessionError>(owner.setSampleRate(44100.0)).code == project::SessionErrorCode::recordingActive);
    REQUIRE(std::get<project::SessionError>(owner.newProject()).code == project::SessionErrorCode::recordingActive);
    REQUIRE(std::get<project::SessionError>(owner.save(fixture.scratch.directory.getChildFile("pending.composer"))).code == project::SessionErrorCode::recordingActive);
    REQUIRE(std::get<project::SessionError>(owner.playFromStart()).code == project::SessionErrorCode::recordingActive);
    REQUIRE_FALSE(fixture.scratch.directory.getChildFile("pending.composer").exists());
    project::MidiCaptureBuffer smallBuffer(document.events.size() - 1);
    REQUIRE(smallBuffer.start() == project::CaptureStartResult::started);
    for (std::size_t index = 0; index < document.events.size(); ++index)
    {
        const auto& event = document.events[index];
        const auto submitted = smallBuffer.submit(event.timeSeconds, event.bytes);
        REQUIRE(submitted == (index + 1 == document.events.size()
            ? project::CaptureSubmitResult::overflow : project::CaptureSubmitResult::accepted));
    }
    auto truncated = smallBuffer.finish();
    REQUIRE(truncated.has_value());
    REQUIRE_FALSE(truncated->isComplete());
    REQUIRE(truncated->overflowEvents == 1);
    const auto retainedEvents = truncated->events;
    const auto incomplete = owner.commitRecording(*ticket, *truncated, document.durationSeconds);
    REQUIRE(std::get<app::ApplicationError>(incomplete).code == app::ApplicationErrorCode::incompleteCapture);
    REQUIRE(owner.session().isRecording());
    empty.requireUnchanged(owner);
    REQUIRE(truncated->events == retainedEvents);
    REQUIRE(completed.events == document.events);
    REQUIRE(std::holds_alternative<project::ProjectError>(owner.commitRecording(*ticket, completed, 0.1)));
    empty.requireUnchanged(owner);
    REQUIRE(owner.session().isRecording());
    requireApplied(owner.commitRecording(*ticket, completed, document.durationSeconds));
    REQUIRE_FALSE(owner.session().isRecording());
    REQUIRE(owner.session().document() == document);
    REQUIRE(owner.session().context().revision == empty.context.revision + 1);
    REQUIRE(completed.events == document.events);
    REQUIRE(render(fixture, 1.0).peak > 0.001);
    REQUIRE(std::get<project::SessionError>(owner.beginRecording()).code == project::SessionErrorCode::existingPerformance);
}

TEST_CASE("Application cancellation and empty completion preserve the clean document", "[app][application-project]")
{
    Fixture fixture;
    auto& owner = *fixture.owner;
    const Snapshot empty(owner);
    const auto cancelled = beginRecording(owner);
    requireOk(owner.cancelRecording(*cancelled));
    empty.requireUnchanged(owner);
    REQUIRE_FALSE(owner.session().isRecording());
    REQUIRE(std::get<project::SessionError>(owner.commitRecording(*cancelled, {}, 0.0)).code == project::SessionErrorCode::noRecording);
    const auto ticket = beginRecording(owner);
    const auto completed = owner.commitRecording(*ticket, {}, 0.0);
    REQUIRE(std::get<contracts::EditOutcome>(completed) == contracts::EditOutcome::noChange);
    REQUIRE_FALSE(owner.session().isRecording());
    REQUIRE_FALSE(owner.session().isDirty());
    REQUIRE(owner.session().document() == empty.document);
    REQUIRE(owner.session().context().revision == empty.context.revision);
    REQUIRE(owner.playbackConversion().clips.empty());
    const auto next = beginRecording(owner);
    requireOk(owner.cancelRecording(*next));
}

TEST_CASE("Application rejects retained completion and cancellation from an earlier take", "[app][application-project]")
{
    Fixture fixture;
    auto& owner = *fixture.owner;
    const auto document = shortDocument();
    const auto first = beginRecording(owner);
    const auto captured = capture(document);
    requireOk(owner.cancelRecording(*first));
    const auto second = beginRecording(owner);
    REQUIRE(first->context.projectInstanceId == second->context.projectInstanceId);
    REQUIRE(first->context.revision == second->context.revision);
    REQUIRE(first->attempt != second->attempt);
    const Snapshot pending(owner);
    REQUIRE(std::get<project::SessionError>(owner.commitRecording(*first, captured, document.durationSeconds)).code == project::SessionErrorCode::obsoleteRecording);
    pending.requireUnchanged(owner);
    REQUIRE(owner.session().isRecording());
    REQUIRE(std::get<project::SessionError>(owner.cancelRecording(*first)).code == project::SessionErrorCode::obsoleteRecording);
    pending.requireUnchanged(owner);
    REQUIRE(owner.session().isRecording());
    REQUIRE(captured.events == document.events);
    requireOk(owner.cancelRecording(*second));

    requireOk(owner.newProject());
    const auto current = beginRecording(owner);
    REQUIRE(current->context.projectInstanceId != first->context.projectInstanceId);
    const Snapshot replacement(owner);
    REQUIRE(std::get<contracts::ContractError>(owner.commitRecording(*first, captured, document.durationSeconds)).code == contracts::ErrorCode::wrongProject);
    replacement.requireUnchanged(owner);
    REQUIRE(owner.session().isRecording());
    REQUIRE(std::get<contracts::ContractError>(owner.cancelRecording(*first)).code == contracts::ErrorCode::wrongProject);
    replacement.requireUnchanged(owner);
    REQUIRE(owner.session().isRecording());
    REQUIRE(captured.events == document.events);
    const auto currentCapture = capture(document);
    requireApplied(owner.commitRecording(*current, currentCapture, document.durationSeconds));
    REQUIRE(owner.session().document() == document);
    REQUIRE_FALSE(owner.session().isRecording());
    REQUIRE(render(fixture, 1.0).peak > 0.001);
}

TEST_CASE("Application file failures preserve the dirty document and playable graph", "[app][application-project]")
{
    Fixture fixture;
    auto& owner = *fixture.owner;
    installTake(owner, shortDocument());
    const auto original = render(fixture, 1.0);
    const Snapshot snapshot(owner);
    REQUIRE(std::holds_alternative<project::ProjectFileError>(owner.save(fixture.scratch.directory)));
    snapshot.requireUnchanged(owner);
    const auto malformed = fixture.scratch.directory.getChildFile("malformed.composer");
    REQUIRE(malformed.replaceWithText("{\"schema_version\":"));
    REQUIRE(std::holds_alternative<project::ProjectFileError>(owner.open(malformed)));
    snapshot.requireUnchanged(owner);
    REQUIRE(std::holds_alternative<project::ProjectFileError>(owner.open(fixture.scratch.directory.getChildFile("missing.composer"))));
    snapshot.requireUnchanged(owner);
    REQUIRE(std::get<project::SessionError>(owner.newProject()).code == project::SessionErrorCode::unsavedChanges);
    snapshot.requireUnchanged(owner);
    const auto otherPath = fixture.scratch.directory.getChildFile("other.composer");
    REQUIRE(std::holds_alternative<project::SaveReceipt>(project::saveProjectFile(otherPath, {})));
    REQUIRE(std::get<project::SessionError>(owner.open(otherPath)).code == project::SessionErrorCode::unsavedChanges);
    snapshot.requireUnchanged(owner);
    REQUIRE(identicalAudio(original, render(fixture, 1.0)));
    const auto savedPath = fixture.scratch.directory.getChildFile("saved.composer");
    REQUIRE(std::holds_alternative<project::SaveReceipt>(owner.save(savedPath)));
    REQUIRE_FALSE(owner.session().isDirty());
    const Snapshot clean(owner);
    REQUIRE(std::holds_alternative<project::ProjectFileError>(owner.open(malformed)));
    clean.requireUnchanged(owner);
    requireOk(owner.newProject());
    REQUIRE(owner.session().context().projectInstanceId != clean.context.projectInstanceId);
    REQUIRE(owner.session().context().revision == 0);
    REQUIRE(owner.session().document() == project::ProjectDocument{});
    REQUIRE(owner.playbackConversion().clips.empty());
    requireOk(owner.open(savedPath));
    REQUIRE(owner.session().document() == snapshot.document);
    REQUIRE(identicalAudio(original, render(fixture, 1.0)));
}

TEST_CASE("Application rate changes rebuild only stopped playback and preserve the document", "[app][application-project]")
{
    Fixture fixture;
    auto& owner = *fixture.owner;
    installTake(owner, shortDocument());
    const Snapshot snapshot(owner);
    REQUIRE(std::get<app::ApplicationError>(owner.setSampleRate(std::numeric_limits<double>::quiet_NaN())).code == app::ApplicationErrorCode::invalidSampleRate);
    snapshot.requireUnchanged(owner);
    REQUIRE(std::get<app::ApplicationError>(owner.setSampleRate(std::numeric_limits<double>::max())).code == app::ApplicationErrorCode::playbackConstructionFailed);
    snapshot.requireUnchanged(owner);
    REQUIRE(owner.playbackSampleRate() == initialRate);
    requireOk(owner.setSampleRate(initialRate));
    snapshot.requireUnchanged(owner);
    requireOk(owner.playFromStart());
    REQUIRE(owner.playbackEdit().getTransport().isPlaying());
    REQUIRE(std::get<app::ApplicationError>(owner.setSampleRate(44100.0)).code == app::ApplicationErrorCode::transportActive);
    auto patch = snapshot.document.patch;
    patch.gainDb = -24.0;
    REQUIRE(std::get<app::ApplicationError>(owner.apply(commandFor(owner, patch))).code == app::ApplicationErrorCode::transportActive);
    snapshot.requireUnchanged(owner);
    owner.stop();
    requireOk(owner.setSampleRate(44100.0));
    REQUIRE(owner.playbackSampleRate() == 44100.0);
    REQUIRE(owner.session().document() == snapshot.document);
    REQUIRE(owner.session().context().revision == snapshot.context.revision);
    REQUIRE(owner.session().context().projectInstanceId == snapshot.context.projectInstanceId);
    REQUIRE(owner.session().isDirty() == snapshot.dirty);
    REQUIRE(owner.playbackPatch() == snapshot.document.patch);
    fixture.engine->getDeviceManager().getHostedAudioDeviceInterface().prepareToPlay(44100.0, blockSize);
    fixture.engine->getDeviceManager().dispatchPendingUpdates();
    REQUIRE(render(fixture, 1.0).audio.size() == 44100);
    REQUIRE(owner.session().document() == snapshot.document);
}
