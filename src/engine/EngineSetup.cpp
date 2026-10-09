#include <composer/engine/EngineSetup.h>

#include <tracktion_engine/tracktion_engine.h>

namespace composer::engine
{
namespace
{

class ScratchPropertyStorage final : public tracktion::PropertyStorage
{
public:
    ScratchPropertyStorage(const juce::String& applicationName, juce::File scratchDirectory)
        : tracktion::PropertyStorage(applicationName), root(std::move(scratchDirectory))
    {
    }

    juce::File getAppCacheFolder() override
    {
        return root.getChildFile("cache");
    }

    juce::File getAppPrefsFolder() override
    {
        return root.getChildFile("preferences");
    }

    void removeProperty(tracktion::SettingID) override
    {
    }

    juce::var getProperty(tracktion::SettingID, const juce::var& defaultValue) override
    {
        return defaultValue;
    }

    void setProperty(tracktion::SettingID, const juce::var&) override
    {
    }

    std::unique_ptr<juce::XmlElement> getXmlProperty(tracktion::SettingID) override
    {
        return {};
    }

    void setXmlProperty(tracktion::SettingID, const juce::XmlElement&) override
    {
    }

    void removePropertyItem(tracktion::SettingID, juce::StringRef) override
    {
    }

    juce::var getPropertyItem(tracktion::SettingID, juce::StringRef, const juce::var& defaultValue) override
    {
        return defaultValue;
    }

    void setPropertyItem(tracktion::SettingID, juce::StringRef, const juce::var&) override
    {
    }

    std::unique_ptr<juce::XmlElement> getXmlPropertyItem(tracktion::SettingID, juce::StringRef) override
    {
        return {};
    }

    void setXmlPropertyItem(tracktion::SettingID, juce::StringRef, const juce::XmlElement&) override
    {
    }

private:
    juce::File root;
};

class ApplicationPropertyStorage final : public tracktion::PropertyStorage
{
public:
    using tracktion::PropertyStorage::PropertyStorage;

    // Settings stay in the roaming profile; caches and temporary renders are machine-local.
    juce::File getAppCacheFolder() override
    {
       #if JUCE_WINDOWS
        auto folder = juce::File::getSpecialLocation(juce::File::windowsLocalAppData)
                          .getChildFile(getApplicationName())
                          .getChildFile("Cache");

        if (! folder.isDirectory())
            folder.createDirectory();

        return folder;
       #else
        return tracktion::PropertyStorage::getAppCacheFolder();
       #endif
    }
};

class ApplicationEngineBehaviour final : public tracktion::EngineBehaviour
{
public:
    // Audio inputs, such as a microphone, open only when a feature asks for them.
    bool shouldOpenAudioInputByDefault() override
    {
        return false;
    }
};

class HeadlessEngineBehaviour final : public tracktion::EngineBehaviour
{
public:
    bool autoInitialiseDeviceManager() override
    {
        return false;
    }

    bool addSystemAudioIODeviceTypes() override
    {
        return false;
    }
};

} // namespace

std::unique_ptr<tracktion::Engine> createEngine(const juce::String& applicationName)
{
    return std::make_unique<tracktion::Engine>(std::make_unique<ApplicationPropertyStorage>(applicationName),
                                               std::make_unique<tracktion::UIBehaviour>(),
                                               std::make_unique<ApplicationEngineBehaviour>());
}

std::unique_ptr<tracktion::Engine> createHeadlessEngine(const juce::String& applicationName,
                                                        const juce::File& scratchDirectory)
{
    return std::make_unique<tracktion::Engine>(
        std::make_unique<ScratchPropertyStorage>(applicationName, scratchDirectory),
        std::make_unique<tracktion::UIBehaviour>(),
        std::make_unique<HeadlessEngineBehaviour>());
}

} // namespace composer::engine
