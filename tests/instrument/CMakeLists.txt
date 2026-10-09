composer_add_test_executable(composer_instrument_tests
    SOURCES
        InstrumentProcessorTests.cpp)

composer_link_instrument(composer_instrument_tests)
