add_subdirectory(synth)

# The JUCE adapter around the synthesiser compiles into every product and test target that uses
# it, under that target's own JUCE configuration, so the plugin and the application never share
# JUCE object code built with different settings. It only routes buffers, MIDI, parameters and
# state; all signal processing is in composer::instrument_synth, which every consumer shares.
# Consumers provide JUCE and call composer_link_instrument.
add_library(composer_instrument INTERFACE)
add_library(composer::instrument ALIAS composer_instrument)

target_sources(composer_instrument
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/InstrumentProcessor.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/PatchParameter.cpp")

target_include_directories(composer_instrument
    INTERFACE
        "${PROJECT_SOURCE_DIR}/include")

target_link_libraries(composer_instrument
    INTERFACE
        composer::instrument_synth)
