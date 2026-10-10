cmake_minimum_required(VERSION 3.25)

# Builds Steinberg's VST 3 validator and a small render host from the pinned VST 3 SDK sources.
# The main build configures this as a separate project, so the SDK's own compiler and output
# settings never reach Composer's targets. VST3_SDK_ROOT holds the SDK's base, pluginterfaces,
# public.sdk and cmake directories side by side, as the SDK's own build expects.

if(NOT VST3_SDK_ROOT)
    message(FATAL_ERROR "Set VST3_SDK_ROOT to the assembled VST 3 SDK sources")
endif()

file(TO_CMAKE_PATH "${VST3_SDK_ROOT}" VST3_SDK_ROOT)

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF)

list(APPEND CMAKE_MODULE_PATH "${VST3_SDK_ROOT}/cmake/modules")
include(SMTG_VST3_SDK)

project(ComposerVst3Host LANGUAGES C CXX)

smtg_setup_platform_toolset()
smtg_setup_symbol_visibility()

set(SDK_ROOT "${VST3_SDK_ROOT}")
set(public_sdk_SOURCE_DIR "${SDK_ROOT}/public.sdk")
set(pluginterfaces_SOURCE_DIR "${SDK_ROOT}/pluginterfaces")
set(VST_SDK TRUE)
include_directories("${SDK_ROOT}")

smtg_create_lib_base_target()
smtg_create_pluginterfaces_target()
smtg_create_public_sdk_common_target()
smtg_create_public_sdk_hosting_target()

add_subdirectory("${SDK_ROOT}/public.sdk/samples/vst-hosting/validator" validator)

# The SDK sources the render host needs keep the SDK's warning level; the render host's own source
# is held to the level of Composer's sources, with warnings as errors.
add_library(composer_vst3_hosting STATIC
    "${SDK_ROOT}/public.sdk/source/common/memorystream.cpp"
    "${SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp"
    "${SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp")
target_compile_features(composer_vst3_hosting PUBLIC cxx_std_17)
target_link_libraries(composer_vst3_hosting PUBLIC sdk_hosting)

add_executable(composer_vst3_render render.cpp)
get_target_property(composer_vst3_render_options composer_vst3_render COMPILE_OPTIONS)
list(REMOVE_ITEM composer_vst3_render_options /W3)
set_property(TARGET composer_vst3_render PROPERTY COMPILE_OPTIONS ${composer_vst3_render_options} /W4 /WX)
target_link_libraries(composer_vst3_render PRIVATE composer_vst3_hosting)
