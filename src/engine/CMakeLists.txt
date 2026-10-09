# One static library holds the JUCE and Tracktion Engine modules for the application and the
# tests that drive it, so the large sequencing engine is compiled once. Targets that link it
# must not also link JUCE module targets: the modules' include directories and definitions are
# re-exported from here instead.
add_library(composer_engine STATIC)
add_library(composer::engine ALIAS composer_engine)

set(composer_engine_sources
    EngineSetup.cpp)

target_sources(composer_engine PRIVATE ${composer_engine_sources})
composer_apply_warnings(composer_engine ${composer_engine_sources})

target_link_libraries(composer_engine
    PRIVATE
        tracktion::tracktion_engine
        tracktion::tracktion_graph
        tracktion::tracktion_core
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)

target_compile_definitions(composer_engine
    PUBLIC
        JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        # Debug builds report every failed assertion through juce::Logger, where the test
        # runner turns it into a failure.
        $<$<CONFIG:Debug>:JUCE_LOG_ASSERTIONS=1>
    INTERFACE
        $<TARGET_PROPERTY:composer_engine,COMPILE_DEFINITIONS>)

set(composer_include_directory "${PROJECT_SOURCE_DIR}/include")
target_include_directories(composer_engine
    PUBLIC
        "${composer_include_directory}")

# Third-party include directories are system directories for consumers, so their headers are
# not held to Composer's warning policy; Composer's own headers stay outside that exemption.
string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" composer_include_pattern "${composer_include_directory}")
target_include_directories(composer_engine SYSTEM
    INTERFACE
        "$<FILTER:$<TARGET_PROPERTY:composer_engine,INCLUDE_DIRECTORIES>,EXCLUDE,^${composer_include_pattern}$>")
