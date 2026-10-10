#include "HostedPluginState.h"

namespace composer::tests
{

juce::MemoryBlock makeHostedState(const std::string& bytes)
{
    juce::XmlElement state("VST3PluginState");
    const juce::MemoryBlock component(bytes.data(), bytes.size());
    state.createNewChildElement("IComponent")->addTextElement(component.toBase64Encoding());

    juce::MemoryBlock hosted;
    juce::AudioProcessor::copyXmlToBinary(state, hosted);
    return hosted;
}

std::optional<std::string> componentBytes(const juce::MemoryBlock& hostedState)
{
    const auto xml = juce::AudioProcessor::getXmlFromBinary(hostedState.getData(),
                                                           static_cast<int>(hostedState.getSize()));

    if (xml == nullptr)
        return std::nullopt;

    const auto* component = xml->getChildByName("IComponent");

    if (component == nullptr)
        return std::nullopt;

    juce::MemoryBlock bytes;

    if (! bytes.fromBase64Encoding(component->getAllSubText()))
        return std::nullopt;

    return std::string(static_cast<const char*>(bytes.getData()), bytes.getSize());
}

} // namespace composer::tests
