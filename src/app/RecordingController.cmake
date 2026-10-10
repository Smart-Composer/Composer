# The controller shares each consumer's application-side engine configuration.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/RecordingCapture.cmake")

add_library(composer_recording_controller INTERFACE)
add_library(composer::recording_controller ALIAS composer_recording_controller)

target_sources(composer_recording_controller INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/RecordingController.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/RecordingDuration.cpp")
target_include_directories(composer_recording_controller INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

function(composer_link_recording_controller target)
    target_link_libraries(${target} PRIVATE composer::recording_controller)
    composer_link_application_project(${target})
    composer_link_recording_capture(${target})
    get_target_property(sources composer_recording_controller INTERFACE_SOURCES)
    composer_apply_warnings(${target} ${sources})
endfunction()
