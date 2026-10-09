# The upper bound keeps policy behaviour identical across the CMake 4.x releases that Visual
# Studio bundles and that hosted runners install.
cmake_minimum_required(VERSION 3.28...4.1)

project(Composer
    VERSION 0.1.0
    DESCRIPTION "A local digital audio workstation with a shared instrument engine"
    LANGUAGES C CXX)

if(NOT PROJECT_IS_TOP_LEVEL)
    message(FATAL_ERROR "Composer is built as a top-level project.")
endif()

option(COMPOSER_BUILD_TESTS "Build Composer's tests and register them with CTest" ON)
option(COMPOSER_WARNINGS_AS_ERRORS "Treat compiler warnings in Composer's own sources as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Composer does not use C++20 modules; skip the per-source module dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Composer builds for x64 only. Configure from an x64 developer environment, "
                            "for example through scripts/verify.ps1; a binary directory already "
                            "configured for another architecture must be removed first (-Clean).")
    endif()

    # Link the C++ runtime statically everywhere. The VST3 runs inside other hosts' processes
    # and must not depend on the runtime version a host happens to load.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ComposerDependencies)
include(ComposerTargets)

if(COMPOSER_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if(COMPOSER_BUILD_TESTS)
    add_subdirectory(tests)
endif()
