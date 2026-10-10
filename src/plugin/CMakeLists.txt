# The VST3 instrument. Its plugin and manufacturer codes form the identity hosts store in their
# sessions; they must never change once a release has shipped.
juce_add_plugin(ComposerInstrument
    PRODUCT_NAME "Composer Instrument"
    COMPANY_NAME "Composer"
    BUNDLE_ID "io.github.smart-composer.instrument"
    DESCRIPTION "The Composer instrument"
    PLUGIN_MANUFACTURER_CODE Cmps
    PLUGIN_CODE Cins
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Synth
    COPY_PLUGIN_AFTER_BUILD FALSE)

set(composer_plugin_sources
    PluginEntry.cpp)

target_sources(ComposerInstrument PRIVATE ${composer_plugin_sources})
composer_apply_warnings(ComposerInstrument ${composer_plugin_sources})
composer_link_instrument(ComposerInstrument)

# The parameter-ID settings fix how hosts identify each automatable parameter and how MIDI
# controllers reach the instrument. They equal JUCE's defaults; they are pinned because changing
# any of them would remap automation in saved host sessions.
target_compile_definitions(ComposerInstrument
    PUBLIC
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_FORCE_USE_LEGACY_PARAM_IDS=0
        JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1
        JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=1)

target_link_libraries(ComposerInstrument
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags)
