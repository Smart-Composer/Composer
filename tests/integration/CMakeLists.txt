# Steinberg's validator and the SDK-based render host, built as a separate project from the
# pinned VST 3 SDK, always optimised: they are tools, not code under test. That project is built
# with Ninja and the same compiler, so the main build must use Ninja too.
if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
    message(FATAL_ERROR "Composer's tests need a Ninja generator, as the presets use; "
        "configure with -DCOMPOSER_BUILD_TESTS=OFF to build without them")
endif()

include(ExternalProject)
set(composer_vst3_tools "${CMAKE_BINARY_DIR}/v3")
set(composer_vst3_validator "${composer_vst3_tools}/bin/validator${CMAKE_EXECUTABLE_SUFFIX}")
set(composer_vst3_render "${composer_vst3_tools}/bin/composer_vst3_render${CMAKE_EXECUTABLE_SUFFIX}")
ExternalProject_Add(composer_vst3_tools
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/tests/vst3host"
    BINARY_DIR "${composer_vst3_tools}"
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DVST3_SDK_ROOT=${COMPOSER_VST3_SDK_DIR}"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${composer_vst3_tools}" --target validator composer_vst3_render
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS "${composer_vst3_validator}" "${composer_vst3_render}"
    INSTALL_COMMAND "")

# The plug-in bundle, two levels above the module inside it.
set(composer_vst3_bundle "$<PATH:GET_PARENT_PATH,$<PATH:GET_PARENT_PATH,$<TARGET_FILE_DIR:ComposerInstrument_VST3>>>")

add_test(NAME "Steinberg's validator passes the VST3, including its extensive tests"
    COMMAND "${composer_vst3_validator}" -e "${composer_vst3_bundle}")
# The validator also exits successfully when it could not set up a test, or ran none.
set_tests_properties("Steinberg's validator passes the VST3, including its extensive tests"
    PROPERTIES
        TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "Failed to setup test;Failed to teardown test;Result: 0 tests passed")

composer_add_test_executable(composer_integration_tests
    SOURCES
        EngineRenderTests.cpp
        ExternalHostTests.cpp
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

add_dependencies(composer_integration_tests ComposerInstrument_VST3 composer_vst3_tools)
target_compile_definitions(composer_integration_tests
    PRIVATE
        COMPOSER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        COMPOSER_TEST_VST3_MODULE="$<TARGET_FILE:ComposerInstrument_VST3>"
        COMPOSER_TEST_VST3_BUNDLE="${composer_vst3_bundle}"
        COMPOSER_TEST_VST3_RENDER_HOST="${composer_vst3_render}")

add_test(NAME "The Composer application starts and runs its startup check"
    COMMAND Composer --startup-check)
set_tests_properties("The Composer application starts and runs its startup check"
    PROPERTIES TIMEOUT 120)
