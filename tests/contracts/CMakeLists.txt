set(contract_test_sources
    ContractsTests.cpp
    ContextRevisionTests.cpp
    CodecHardeningTests.cpp
    ContractDiagnosticsTests.cpp)
add_executable(composer_contracts_tests ${contract_test_sources})
target_compile_features(composer_contracts_tests PRIVATE cxx_std_20)
target_link_libraries(composer_contracts_tests PRIVATE
    composer::contracts Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
target_compile_definitions(composer_contracts_tests PRIVATE
    COMPOSER_CONTRACT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
composer_apply_warnings(composer_contracts_tests ${contract_test_sources})
catch_discover_tests(composer_contracts_tests DISCOVERY_MODE PRE_TEST)
