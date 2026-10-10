composer_add_test_executable(composer_instrument_tests
    SOURCES
        InstrumentProcessorTests.cpp
        PatchInterfaceTests.cpp
        ProcessorRenderTests.cpp
        ../support/AllocationProbe.cpp
        ../support/PatchFixtures.cpp
        ../support/RenderHarness.cpp)

composer_link_instrument(composer_instrument_tests)
target_link_libraries(composer_instrument_tests PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(composer_instrument_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
target_compile_definitions(composer_instrument_tests PRIVATE COMPOSER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# The synthesiser core, tested without JUCE.
set(composer_instrument_synth_test_sources
    synth/SynthTestMain.cpp
    synth/Spectrum.cpp
    synth/ParameterTests.cpp
    synth/DspTests.cpp
    synth/SynthTests.cpp
    synth/PatchStateOrderTests.cpp
    "${PROJECT_SOURCE_DIR}/tests/support/TestProcess.cpp")

add_executable(composer_instrument_synth_tests ${composer_instrument_synth_test_sources})
target_link_libraries(composer_instrument_synth_tests PRIVATE composer::instrument_synth Catch2::Catch2)
target_include_directories(composer_instrument_synth_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/instrument/synth"
        "${PROJECT_SOURCE_DIR}/tests/support")
composer_apply_warnings(composer_instrument_synth_tests ${composer_instrument_synth_test_sources})

catch_discover_tests(composer_instrument_synth_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 300)
