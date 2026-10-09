find_package(Threads REQUIRED)

add_library(composer_project_document STATIC ProjectDocument.cpp MidiCaptureBuffer.cpp ProjectSession.cpp)
add_library(composer::project_document ALIAS composer_project_document)
target_compile_features(composer_project_document PUBLIC cxx_std_20)
target_include_directories(composer_project_document PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(composer_project_document
    PUBLIC composer::contracts Threads::Threads
    PRIVATE nlohmann_json::nlohmann_json)
composer_apply_warnings(composer_project_document ProjectDocument.cpp MidiCaptureBuffer.cpp ProjectSession.cpp)

add_library(composer_project STATIC ProjectFileStore.cpp)
add_library(composer::project ALIAS composer_project)
target_link_libraries(composer_project PUBLIC composer::project_document composer::engine)
composer_apply_warnings(composer_project ProjectFileStore.cpp)
