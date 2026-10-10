include_guard(GLOBAL)

# Every dependency is a pinned source archive, either a GitHub archive of an exact commit or a
# release asset, verified by its SHA-256. To move a pin, change the URL and the hash together,
# record the reason in docs/dependencies.md, and verify from a clean binary directory
# (scripts/verify.ps1 -Clean).

include(FetchContent)

# JUCE 9.0.3 (release tag). AGPL-3.0-only for open-source use. PatchJuce.cmake applies Composer's
# repairs to it.
FetchContent_Declare(juce
    URL https://github.com/juce-framework/JUCE/archive/be29c81492b6151c8ea8d14c840e1311963b3a83.tar.gz
    URL_HASH SHA256=f846e74503e5b959b1de514c772b05ab1cb05478b7513b19d6294cb2b3d2906a
    PATCH_COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/PatchJuce.cmake"
    SYSTEM
    EXCLUDE_FROM_ALL)

# Tracktion Engine, develop branch. GPL-3.0-or-later for open-source use. Only the modules
# directory is configured; its bundled JUCE submodule is not part of the archive and the
# JUCE pin above is used instead.
FetchContent_Declare(tracktion_engine
    URL https://github.com/Tracktion/tracktion_engine/archive/e42d82f06e553771477eabd1dd0d561fe2cbe573.tar.gz
    URL_HASH SHA256=cd8571a1b0ea5e70e342c3315272adc282b505eefa39fc7e7acebd94484dd5c5
    SOURCE_SUBDIR modules
    SYSTEM
    EXCLUDE_FROM_ALL)

# nlohmann/json 3.12.0 (release asset). MIT.
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SYSTEM
    EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(juce tracktion_engine nlohmann_json)

if(COMPOSER_BUILD_TESTS)
    # Catch2 3.16.1 (release tag). BSL-1.0.
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/08092139210881f01a34e60c5f2fd6afa0ea4024.tar.gz
        URL_HASH SHA256=c66daf9f31712f673ebdffb074c90b5d7e61ea50adc6a1d2db95e798af027940
        SYSTEM
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")

    # Steinberg VST 3 SDK 3.8.0 (tag v3.8.0_build_66), the SDK version JUCE 9.0.3 bundles. MIT.
    # Test tooling only: Steinberg's validator and a render host on the SDK's hosting library
    # check the built plug-in in a host other than JUCE. Only the four parts that tooling needs
    # are fetched, side by side in one directory as the SDK's build expects; tests/vst3host
    # builds them as a separate project, so none of this is configured here.
    set(COMPOSER_VST3_SDK_DIR "${FETCHCONTENT_BASE_DIR}/vst3sdk")
    FetchContent_Declare(vst3_base
        URL https://github.com/steinbergmedia/vst3_base/archive/3d2e82f8e6bff59c1d8b7a27491a29c2286b5206.tar.gz
        URL_HASH SHA256=8f1d5f9ac0cd1e916ca3a196f5ee080ec4301b0b10a7ee7e564013f5d58b03cd
        SOURCE_DIR "${COMPOSER_VST3_SDK_DIR}/base"
        SOURCE_SUBDIR not-configured-here)
    FetchContent_Declare(vst3_pluginterfaces
        URL https://github.com/steinbergmedia/vst3_pluginterfaces/archive/31d6eeba6daaa3e2a8bfbe3e7a90ca0b7fbfbc1c.tar.gz
        URL_HASH SHA256=7c9d19af0e81711edde34c3eb2e5e6d150ae1f449501bb5b352605b9590654d7
        SOURCE_DIR "${COMPOSER_VST3_SDK_DIR}/pluginterfaces"
        SOURCE_SUBDIR not-configured-here)
    FetchContent_Declare(vst3_public_sdk
        URL https://github.com/steinbergmedia/vst3_public_sdk/archive/a3911a4615dabbfdfd9d181ee26b05c70c289a95.tar.gz
        URL_HASH SHA256=4cc8a9a57a970172b1efbef62025ae4f0656e132eed994f0e15ed19dfd9b0bb3
        SOURCE_DIR "${COMPOSER_VST3_SDK_DIR}/public.sdk"
        SOURCE_SUBDIR not-configured-here)
    FetchContent_Declare(vst3_cmake
        URL https://github.com/steinbergmedia/vst3_cmake/archive/de6e54eeaaab35b7145f5c32c279b5e892146e04.tar.gz
        URL_HASH SHA256=ab4ce274563570fed11f2f0a31feb93280cd26543703f90c404a8720c1a486a5
        SOURCE_DIR "${COMPOSER_VST3_SDK_DIR}/cmake"
        SOURCE_SUBDIR not-configured-here)
    FetchContent_MakeAvailable(vst3_base vst3_pluginterfaces vst3_public_sdk vst3_cmake)

    # The SDK's build finds its parts only side by side, so a FETCHCONTENT_SOURCE_DIR_VST3_*
    # override of one part would be ignored; refuse it rather than build from a stale copy.
    foreach(part IN ITEMS base pluginterfaces public.sdk cmake)
        string(REPLACE "." "_" name "${part}")
        get_filename_component(fetched "${vst3_${name}_SOURCE_DIR}" ABSOLUTE)
        get_filename_component(expected "${COMPOSER_VST3_SDK_DIR}/${part}" ABSOLUTE)
        if(NOT fetched STREQUAL expected)
            message(FATAL_ERROR "The VST 3 SDK's ${part} must be fetched into ${COMPOSER_VST3_SDK_DIR}; "
                "overriding the source of one SDK part is not supported")
        endif()
    endforeach()
endif()
