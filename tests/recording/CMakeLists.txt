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

include("${PROJECT_SOURCE_DIR}/src/app/RecordingController.cmake")

composer_add_test_executable(composer_recording_controller_tests
    SOURCES RecordingControllerTests.cpp RecordingDurationTests.cpp)
composer_link_recording_controller(composer_recording_controller_tests)

# Each native input fixture owns its process-wide WinMM redirects.
composer_add_test_executable(composer_recording_device_tests
    SOURCES RecordingDeviceTests.cpp "${PROJECT_SOURCE_DIR}/src/app/RecordingDevice.cpp")
composer_link_application_project(composer_recording_device_tests)

composer_add_test_executable(composer_recording_workflow_tests
    SOURCES RecordingWorkflowTests.cpp "${PROJECT_SOURCE_DIR}/src/app/RecordingDevice.cpp")
composer_link_recording_controller(composer_recording_workflow_tests)
