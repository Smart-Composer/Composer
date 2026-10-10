#include <composer/engine/EngineSetup.h>
#include <composer/instrument/InstrumentProcessor.h>
#include "RecordingWorkspace.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <cmath>
#include <limits>

namespace composer::app
{
namespace
{

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow(const juce::String& name, tracktion::Engine& engine)
        : juce::DocumentWindow(name,
                               juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                                   juce::ResizableWindow::backgroundColourId),
                               juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        auto workspace = std::make_unique<RecordingWorkspace>(engine);
        workspace->setSize(1000, 800);
        setContentOwned(workspace.release(), true);
        setResizable(true, true);
        setResizeLimits(900, 650, 2400, 1600);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    void requestClose(std::function<void()> close)
    {
        if (auto* workspace = dynamic_cast<RecordingWorkspace*>(getContentComponent()))
            workspace->requestClose(std::move(close));
    }
};

/** Builds the recording workspace without devices and runs the shared instrument
    for one block. Used by automated checks to prove the built application starts. */
bool runStartupCheck(const juce::String& applicationName)
{
    const auto scratch = juce::File::createTempFile("composer-startup-check");

    if (! scratch.createDirectory())
        return false;

    bool passed = false;

    {
        auto engine = composer::engine::createHeadlessEngine(applicationName, scratch);
        RecordingWorkspace workspace(*engine);
        workspace.setSize(1000, 800);

        composer::instrument::InstrumentProcessor instrument;
        constexpr int blockSize = 128;
        instrument.setRateAndBufferSizeDetails(48000.0, blockSize);
        instrument.prepareToPlay(48000.0, blockSize);

        // NaN in every sample: the check passes only if the instrument writes its whole output.
        juce::AudioBuffer<float> buffer(2, blockSize);

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            juce::FloatVectorOperations::fill(buffer.getWritePointer(channel),
                                              std::numeric_limits<float>::quiet_NaN(),
                                              blockSize);

        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        instrument.processBlock(buffer, midi);
        instrument.releaseResources();

        bool outputWritten = true;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int index = 0; index < blockSize; ++index)
                outputWritten = outputWritten && std::isfinite(buffer.getSample(channel, index));

        const auto edits = engine->getActiveEdits().getEdits();
        passed = edits.size() == 1
              && ! tracktion::getAudioTracks(*edits[0]).isEmpty()
              && outputWritten;
    }

    scratch.deleteRecursively();
    return passed;
}

class ComposerApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override
    {
        return JUCE_APPLICATION_NAME_STRING;
    }

    const juce::String getApplicationVersion() override
    {
        return JUCE_APPLICATION_VERSION_STRING;
    }

    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }

    void initialise(const juce::String& commandLine) override
    {
        if (juce::StringArray::fromTokens(commandLine, true).contains("--startup-check"))
        {
            setApplicationReturnValue(runStartupCheck(getApplicationName()) ? 0 : 1);
            quit();
            return;
        }

        engine = composer::engine::createEngine(getApplicationName());
        mainWindow = std::make_unique<MainWindow>(getApplicationName(), *engine);
    }

    void shutdown() override
    {
        mainWindow.reset();
        engine.reset();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr) mainWindow->requestClose([] { juce::JUCEApplication::quit(); });
        else quit();
    }

    void anotherInstanceStarted(const juce::String&) override
    {
    }

private:
    std::unique_ptr<tracktion::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace
} // namespace composer::app

START_JUCE_APPLICATION(composer::app::ComposerApplication)
