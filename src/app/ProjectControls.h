#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace composer::app
{
class ApplicationProject;

// Message-thread controls for one borrowed project. The component must be
// destroyed before the project. Recording and device ownership stay with the
// application; these callbacks never run on an audio or MIDI callback thread.
class ProjectControls final : public juce::Component
{
public:
    explicit ProjectControls(ApplicationProject&);
    ~ProjectControls() override;

    void refresh();
    void setRecordingStatus(bool busy, bool canStart, bool canRetry, bool canDiscard,
                            juce::String status);
    void resized() override;

    std::function<void()> onRecord;
    std::function<void()> onStopRecording;
    std::function<void()> onRetry;
    std::function<void()> onDiscard;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectControls)
};
}
