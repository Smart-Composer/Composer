include_guard(GLOBAL)

# Every dependency is a pinned source archive, either a GitHub archive of an exact commit or a
# release asset, verified by its SHA-256. To move a pin, change the URL and the hash together,
# record the reason in docs/dependencies.md, and verify from a clean binary directory
# (scripts/verify.ps1 -Clean).

include(FetchContent)

# JUCE 9.0.3 (release tag). AGPL-3.0-only for open-source use.
FetchContent_Declare(juce
    URL https://github.com/juce-framework/JUCE/archive/be29c81492b6151c8ea8d14c840e1311963b3a83.tar.gz
    URL_HASH SHA256=f846e74503e5b959b1de514c772b05ab1cb05478b7513b19d6294cb2b3d2906a
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
endif()
