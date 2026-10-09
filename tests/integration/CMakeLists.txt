composer_add_test_executable(composer_integration_tests
    SOURCES
        EngineRenderTests.cpp
        PluginHostingTests.cpp)

# The hosting tests load the VST3 module built for the same configuration.
add_dependencies(composer_integration_tests ComposerInstrument_VST3)
target_compile_definitions(composer_integration_tests
    PRIVATE
        COMPOSER_TEST_VST3_MODULE="$<TARGET_FILE:ComposerInstrument_VST3>")

add_test(NAME "The Composer application starts and runs its startup check"
    COMMAND Composer --startup-check)
set_tests_properties("The Composer application starts and runs its startup check"
    PROPERTIES TIMEOUT 120)
