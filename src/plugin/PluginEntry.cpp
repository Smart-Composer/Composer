#include <composer/instrument/InstrumentProcessor.h>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new composer::instrument::InstrumentProcessor();
}
