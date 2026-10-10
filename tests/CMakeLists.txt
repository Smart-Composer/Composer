include(Catch)

# composer_add_test_executable(<target> SOURCES <source>...)
#
# A Catch2 test executable whose main thread is a JUCE message thread. It links the
# application-side JUCE and Tracktion Engine library and registers every test case with CTest.
function(composer_add_test_executable target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES")

    juce_add_console_app(${target} PRODUCT_NAME ${target})

    set(sources ${arg_SOURCES}
        "${PROJECT_SOURCE_DIR}/tests/support/JuceTestMain.cpp"
        "${PROJECT_SOURCE_DIR}/tests/support/TestProcess.cpp")
    target_sources(${target} PRIVATE ${sources})
    composer_apply_warnings(${target} ${sources})

    target_link_libraries(${target} PRIVATE composer::engine Catch2::Catch2)

    catch_discover_tests(${target}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 300)
endfunction()

add_subdirectory(instrument)
add_subdirectory(integration)
add_subdirectory(contracts)
add_subdirectory(project)
add_subdirectory(app)
