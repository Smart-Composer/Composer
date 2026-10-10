#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace composer::app
{
// The actual publisher's signals, read directly by MIDI admission. No copy or
// shadow clock is allowed between this state and the capture endpoint.
struct CaptureClockSignals
{
    std::atomic<std::uint64_t> generation{0}, serial{0};
    std::atomic<double> correction{std::numeric_limits<double>::quiet_NaN()};
};

class CaptureClockWitness final : public tracktion::Plugin
{
public:
    static constexpr const char* xmlTypeName = "composer_capture_clock_v1";
    static const char* getPluginName() { return "Recording Clock"; }
    explicit CaptureClockWitness(tracktion::PluginCreationInfo info) : Plugin(info) {}
    ~CaptureClockWitness() override { notifyListenersOfDeletion(); }
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override { return true; }
    bool takesAudioInput() override { return true; }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    void initialise(const tracktion::PluginInitialisationInfo& info) override { rate = info.sampleRate; }
    void deinitialise() override {}
    void applyToBuffer(const tracktion::PluginRenderContext&) override;
    std::atomic<std::uint64_t> serial{0};
    std::atomic<std::int64_t> editEnd{0};

private:
    double rate = 0.0; // Graph lifecycle is quiescent around changes.
};

struct CaptureClockSnapshot
{
    std::uint64_t generation, serial, graphSerial;
    std::int64_t streamEnd, graphEditEnd;
    double rate, correction;
    bool valid;
    // Stream sample that anchored the published correction at block start.
    std::int64_t correctionAnchor = -1;
};

// Install as DeviceManager's global output processor after the instrument graph.
// Witness and input storage are retained. Remove it before destroying their edit
// or engine, and invalidate before changing the selected input's membership.
// All buffers pass through untouched. processBlock reads only the audio thread's
// stream time, input's atomic correction and completed graph witness; no wall clock,
// playback-context pointer, seqlock loop, allocation or project operation.
class CaptureClock final : public juce::AudioProcessor
{
public:
    CaptureClock(tracktion::DeviceManager&, std::shared_ptr<tracktion::MidiInputDevice>,
                 CaptureClockWitness&);
    const CaptureClockSignals& signals() const noexcept { return published; }
    std::optional<CaptureClockSnapshot> read() const noexcept;

    // Message-thread actions lock the physical audio callback. Hosted callers must
    // also serialize their explicit processBlock calls. Neither action opens MIDI
    // admission: a fresh two-point calibration is required for every generation.
    std::optional<std::uint64_t> beginGeneration();
    void invalidate();

    const juce::String getName() const override { return "Recording Clock"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    void invalidateWriter() noexcept;
    tracktion::DeviceManager& devices;
    std::shared_ptr<tracktion::MidiInputDevice> input;
    juce::ReferenceCountedObjectPtr<CaptureClockWitness> graph;
    bool prepared = false, armed = false;
    std::int64_t lastEnd = 0;
    CaptureClockSignals published;
    std::atomic<std::uint64_t> version{0}, graphSerial{0};
    std::atomic<std::int64_t> streamEnd{0}, graphEditEnd{0};
    std::atomic<std::int64_t> correctionAnchor{-1};
    std::atomic<double> rate{0}, copiedCorrection{0};
    std::atomic<bool> valid{false};
};

struct CaptureMapping
{
    std::uint64_t generation, proofSerial;
    double sampleRate;
    std::int64_t streamToEditOffset, takeStartEditSample;
};

// Message-thread calibration: two advancing sync points must each be followed
// by a later completed callback and graph witness. getSyncPoint stays off audio
// and MIDI threads. The context must remain the same, straight and uninterrupted.
// Admission also waits until the correction anchor is beyond the take origin by
// one millisecond, covering whole-millisecond native input timestamps.
class CaptureCalibration final
{
public:
    explicit CaptureCalibration(std::uint64_t generation) : expectedGeneration(generation) {}
    std::optional<CaptureMapping> poll(const CaptureClock&, const tracktion::EditPlaybackContext*);
    bool isContextCurrent(const tracktion::EditPlaybackContext*) const noexcept;

private:
    struct Point
    {
        std::uint64_t fenceSerial, fenceGraphSerial;
        std::int64_t reference, edit;
    };
    void resetPoints() noexcept { pending.reset(); completed.reset(); }
    std::uint64_t expectedGeneration;
    const tracktion::EditPlaybackContext* identity = nullptr;
    std::optional<Point> pending, completed;
};
}
