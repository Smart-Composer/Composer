composer_add_test_executable(composer_integration_tests
    SOURCES
        EngineRenderTests.cpp
        PluginHostingTests.cpp
        PluginEquivalenceTests.cpp
        ../support/HostedPluginState.cpp
        ../support/PatchFixtures.cpp
        ../support/RenderHarness.cpp)

# The equivalence tests compare the instrument compiled into this executable with the VST3
# built in the same configuration.
composer_link_instrument(composer_integration_tests)
target_link_libraries(composer_integration_tests PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(composer_integration_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")

add_dependencies(composer_integration_tests ComposerInstrument_VST3)
target_compile_definitions(composer_integration_tests
    PRIVATE
        COMPOSER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        COMPOSER_TEST_VST3_MODULE="$<TARGET_FILE:ComposerInstrument_VST3>")

add_test(NAME "The Composer application starts and runs its startup check"
    COMMAND Composer --startup-check)
set_tests_properties("The Composer application starts and runs its startup check"
    PROPERTIES TIMEOUT 120)
