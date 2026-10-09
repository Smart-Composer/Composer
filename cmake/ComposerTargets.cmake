include_guard(GLOBAL)

# Composer's warning policy covers Composer's own translation units only. JUCE modules compile
# their sources inside each target that uses them, so a target-wide policy would also hold
# third-party code to it. Headers reached through SYSTEM include directories stay quiet.
if(MSVC)
    set(COMPOSER_WARNING_OPTIONS /W4)
    if(COMPOSER_WARNINGS_AS_ERRORS)
        list(APPEND COMPOSER_WARNING_OPTIONS /WX)
    endif()
else()
    set(COMPOSER_WARNING_OPTIONS -Wall -Wextra -Wpedantic)
    if(COMPOSER_WARNINGS_AS_ERRORS)
        list(APPEND COMPOSER_WARNING_OPTIONS -Werror)
    endif()
endif()

# composer_apply_warnings(<target> <source>...)
#
# Applies the warning policy to the named sources as compiled by <target>, including
# interface sources that another Composer library contributes to it.
function(composer_apply_warnings target)
    set_source_files_properties(${ARGN}
        TARGET_DIRECTORY ${target}
        PROPERTIES COMPILE_OPTIONS "${COMPOSER_WARNING_OPTIONS}")
endfunction()

# composer_link_instrument(<target>)
#
# Compiles the shared instrument sources into <target> under that target's JUCE configuration.
function(composer_link_instrument target)
    target_link_libraries(${target} PRIVATE composer::instrument)
    get_target_property(sources composer_instrument INTERFACE_SOURCES)
    composer_apply_warnings(${target} ${sources})
endfunction()
