# Capture uses application-side JUCE and Tracktion settings in each consumer.
include_guard(GLOBAL)

add_library(composer_recording_capture INTERFACE)
add_library(composer::recording_capture ALIAS composer_recording_capture)

target_sources(composer_recording_capture INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/CaptureClock.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/RecordingCapture.cpp")
target_include_directories(composer_recording_capture INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

function(composer_link_recording_capture target)
    target_link_libraries(${target} PRIVATE composer::recording_capture composer::project composer::engine)
    get_target_property(sources composer_recording_capture INTERFACE_SOURCES)
    composer_apply_warnings(${target} ${sources})
endfunction()
