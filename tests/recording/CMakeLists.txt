include("${PROJECT_SOURCE_DIR}/src/app/RecordingCapture.cmake")

composer_add_test_executable(composer_recording_tests
    SOURCES
        CaptureClockTests.cpp
        CaptureIntegrationTests.cpp
        RecordingCaptureTests.cpp
        ../support/AllocationProbe.cpp)
composer_link_recording_capture(composer_recording_tests)
target_include_directories(composer_recording_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
