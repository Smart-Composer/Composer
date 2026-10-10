# Build the adapter and shared processor with each consumer's own JUCE settings.
add_library(composer_app_instrument INTERFACE)
add_library(composer::app_instrument ALIAS composer_app_instrument)

target_sources(composer_app_instrument INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/InstrumentAdapter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/RenderBridge.cpp")
target_include_directories(composer_app_instrument INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

function(composer_link_app_instrument target)
    target_link_libraries(${target} PRIVATE composer::app_instrument)
    composer_link_instrument(${target})
    get_target_property(sources composer_app_instrument INTERFACE_SOURCES)
    composer_apply_warnings(${target} ${sources})
endfunction()
