# The workspace combines message-thread controls with recording and device ownership.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/RecordingController.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ProjectControls.cmake")

add_library(composer_recording_workspace INTERFACE)
add_library(composer::recording_workspace ALIAS composer_recording_workspace)

target_sources(composer_recording_workspace INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/RecordingDevice.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/RecordingWorkspace.cpp")
target_include_directories(composer_recording_workspace INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

function(composer_link_recording_workspace target)
    target_link_libraries(${target} PRIVATE composer::recording_workspace)
    composer_link_recording_controller(${target})
    composer_link_project_controls(${target})
    get_target_property(sources composer_recording_workspace INTERFACE_SOURCES)
    composer_apply_warnings(${target} ${sources})
endfunction()
