# The synthesiser core: every floating-point operation on audio and parameter values, without
# JUCE. It is compiled once and linked into the application, the plugin and the tests, so they
# all run the same object code and render bit-identical output for the same input. Keep
# value-changing code-generation flags (/fp:fast, /GL, /arch) away from this target.
set(composer_instrument_synth_sources
    ParameterCurves.cpp
    PatchState.cpp
    Synth.cpp
    Wavetables.cpp)

add_library(composer_instrument_synth STATIC ${composer_instrument_synth_sources})
add_library(composer::instrument_synth ALIAS composer_instrument_synth)

target_compile_features(composer_instrument_synth PUBLIC cxx_std_20)
target_include_directories(composer_instrument_synth PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(composer_instrument_synth PUBLIC composer::contracts)

if(MSVC)
    target_compile_options(composer_instrument_synth PRIVATE /fp:precise)
endif()

composer_apply_warnings(composer_instrument_synth ${composer_instrument_synth_sources})
