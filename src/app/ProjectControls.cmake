# Project widgets use the same application-side JUCE settings as their owner.
include_guard(GLOBAL)

add_library(composer_project_controls INTERFACE)
add_library(composer::project_controls ALIAS composer_project_controls)

target_sources(composer_project_controls INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/ProjectControls.cpp")
target_include_directories(composer_project_controls INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

function(composer_link_project_controls target)
    target_link_libraries(${target} PRIVATE composer::project_controls)
    composer_link_application_project(${target})
    get_target_property(sources composer_project_controls INTERFACE_SOURCES)
    composer_apply_warnings(${target} ${sources})
endfunction()
