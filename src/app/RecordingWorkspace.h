#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace tracktion { inline namespace engine { class Engine; } }

namespace composer::app
{
class RecordingWorkspace final : public juce::Component
{
public:
    explicit RecordingWorkspace(tracktion::Engine&);
    ~RecordingWorkspace() override;
    void resized() override;
    void requestClose(std::function<void()>);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecordingWorkspace)
};
}
