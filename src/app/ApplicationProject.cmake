# The owner and graph conversion use each consumer's application-side JUCE settings.
add_library(composer_application_project INTERFACE)
add_library(composer::application_project ALIAS composer_application_project)

target_sources(composer_application_project INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/ApplicationProject.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/DerivedPlayback.cpp")
target_include_directories(composer_application_project INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

function(composer_link_application_project target)
    target_link_libraries(${target} PRIVATE composer::application_project composer::project)
    composer_link_app_instrument(${target})
    get_target_property(sources composer_application_project INTERFACE_SOURCES)
    composer_apply_warnings(${target} ${sources})
endfunction()
