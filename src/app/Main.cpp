#include <composer/engine/EngineSetup.h>
#include <composer/instrument/InstrumentProcessor.h>

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <cmath>
#include <limits>

namespace composer::app
{
namespace
{

class MainComponent final : public juce::Component
{
public:
    MainComponent()
    {
        setSize(800, 500);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
        g.setColour(getLookAndFeel().findColour(juce::Label::textColourId));
        g.setFont(juce::FontOptions(28.0f));
        g.drawText(JUCE_APPLICATION_NAME_STRING, getLocalBounds(), juce::Justification::centred);
    }
};

class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow(const juce::String& name)
        : juce::DocumentWindow(name,
                               juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                                   juce::ResizableWindow::backgroundColourId),
                               juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(), true);
        setResizable(true, true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

/** Starts the sequencing engine without devices, creates an edit and runs the shared
    instrument for one block. Used by automated checks to prove the built application starts. */
bool runStartupCheck(const juce::String& applicationName)
{
    const auto scratch = juce::File::createTempFile("composer-startup-check");

    if (! scratch.createDirectory())
        return false;

    bool passed = false;

    {
        auto engine = composer::engine::createHeadlessEngine(applicationName, scratch);
        auto edit = tracktion::Edit::createSingleTrackEdit(*engine);

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

        passed = edit != nullptr
              && ! tracktion::getAudioTracks(*edit).isEmpty()
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
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
        engine.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
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
