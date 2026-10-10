include(InstrumentAdapter.cmake)
include(ApplicationProject.cmake)

juce_add_gui_app(Composer
    PRODUCT_NAME "Composer"
    COMPANY_NAME "Composer"
    BUNDLE_ID "io.github.smart-composer.composer")

set(composer_app_sources
    Main.cpp)

target_sources(Composer PRIVATE ${composer_app_sources})
composer_apply_warnings(Composer ${composer_app_sources})
composer_link_application_project(Composer)

target_compile_definitions(Composer
    PRIVATE
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Composer,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Composer,JUCE_VERSION>")

target_link_libraries(Composer PRIVATE composer::engine)
