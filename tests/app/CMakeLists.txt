composer_add_test_executable(composer_app_instrument_tests
    SOURCES
        InstrumentAdapterTests.cpp
        ../support/AllocationProbe.cpp)
composer_link_app_instrument(composer_app_instrument_tests)
target_include_directories(composer_app_instrument_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
