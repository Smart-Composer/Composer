# OperatorNewProbe.cpp replaces the global operator new and delete, so it belongs to this
# executable alone.
composer_add_test_executable(composer_instrument_tests
    SOURCES
        InstrumentProcessorTests.cpp
        ParameterIdentityTests.cpp
        PatchInterfaceTests.cpp
        ProcessorRenderTests.cpp
        RealtimeProbeTests.cpp
        ReferencePatches.cpp
        ReferenceRenderTests.cpp
        ReferenceSourceTests.cpp
        ../support/AllocationProbe.cpp
        ../support/OperatorNewProbe.cpp
        ../support/PatchFixtures.cpp
        ../support/RenderHarness.cpp
        ../support/Sha256.cpp
        ../support/SystemActivityProbes.cpp)

composer_link_instrument(composer_instrument_tests)
target_link_libraries(composer_instrument_tests PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(composer_instrument_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
target_compile_definitions(composer_instrument_tests PRIVATE COMPOSER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# The frozen v1 instrument: a copy of the first released instrument in the composer_v1 namespace,
# described in reference/v1/README.md. Its parts are built as the live ones are: the contract and
# the synthesiser core as static libraries with the code-generation settings that composer_contracts
# and composer_instrument_synth had when it was frozen, and the JUCE adapter inside this executable
# beside the live adapter. Both instruments then run on the same CPU with the same runtime, and a
# change to the live code or to its target-level build settings that alters a v1 render shows as a
# difference. Those settings are written out rather than read from the live targets, so they stay
# as frozen. The toolset, JUCE and global compiler flags are shared by both by design, so a change
# there would change both alike; the check at the end of this section refuses the global flags and
# settings known to change floating-point results.
set(composer_reference_v1_dir "${CMAKE_CURRENT_SOURCE_DIR}/reference/v1")

set(composer_reference_v1_contract_sources
    "${composer_reference_v1_dir}/src/contracts/InstrumentPatch.cpp")

add_library(composer_reference_v1_contracts STATIC ${composer_reference_v1_contract_sources})
target_compile_features(composer_reference_v1_contracts PUBLIC cxx_std_20)
target_include_directories(composer_reference_v1_contracts PUBLIC "${composer_reference_v1_dir}/include")
target_link_libraries(composer_reference_v1_contracts PRIVATE nlohmann_json::nlohmann_json)
composer_apply_warnings(composer_reference_v1_contracts ${composer_reference_v1_contract_sources})

set(composer_reference_v1_synth_sources
    "${composer_reference_v1_dir}/src/instrument/synth/ParameterCurves.cpp"
    "${composer_reference_v1_dir}/src/instrument/synth/PatchState.cpp"
    "${composer_reference_v1_dir}/src/instrument/synth/Synth.cpp"
    "${composer_reference_v1_dir}/src/instrument/synth/Wavetables.cpp")

add_library(composer_reference_v1_synth STATIC ${composer_reference_v1_synth_sources})
target_compile_features(composer_reference_v1_synth PUBLIC cxx_std_20)
target_include_directories(composer_reference_v1_synth PUBLIC "${composer_reference_v1_dir}/include")
target_link_libraries(composer_reference_v1_synth PUBLIC composer_reference_v1_contracts)

if(MSVC)
    target_compile_options(composer_reference_v1_synth PRIVATE /fp:precise)
endif()

composer_apply_warnings(composer_reference_v1_synth ${composer_reference_v1_synth_sources})

set(composer_reference_v1_adapter_sources
    "${composer_reference_v1_dir}/src/instrument/InstrumentProcessor.cpp"
    "${composer_reference_v1_dir}/src/instrument/PatchParameter.cpp")

target_sources(composer_instrument_tests PRIVATE ${composer_reference_v1_adapter_sources})
target_link_libraries(composer_instrument_tests PRIVATE composer_reference_v1_synth)
composer_apply_warnings(composer_instrument_tests ${composer_reference_v1_adapter_sources})

# Floating-point code generation that would change the live and the frozen instrument alike:
# instruction-set selection, contraction into fused multiply-adds, fast or strict floating-point
# models, fast transcendentals and whole-program optimisation.
set(composer_shared_build_options "${CMAKE_CXX_FLAGS}")

foreach(configuration IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(APPEND composer_shared_build_options " ${CMAKE_CXX_FLAGS_${configuration}}")
endforeach()

get_directory_property(composer_directory_options COMPILE_OPTIONS)
string(APPEND composer_shared_build_options " ${composer_directory_options}")

foreach(core IN ITEMS composer_instrument_synth composer_reference_v1_synth)
    get_target_property(composer_core_options ${core} COMPILE_OPTIONS)

    if(composer_core_options)
        string(APPEND composer_shared_build_options " ${composer_core_options}")
    endif()

    foreach(property IN ITEMS INTERPROCEDURAL_OPTIMIZATION INTERPROCEDURAL_OPTIMIZATION_DEBUG
                             INTERPROCEDURAL_OPTIMIZATION_RELEASE INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO
                             INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL)
        get_target_property(composer_core_ipo ${core} ${property})

        if(composer_core_ipo)
            message(FATAL_ERROR "${core} has ${property} set, which can change the instrument's floating-point "
                                "results; the frozen v1 comparison cannot detect it.")
        endif()
    endforeach()
endforeach()

foreach(option IN ITEMS arch: fp:fast fp:contract fp:strict GL Qfast_transcendentals)
    foreach(prefix IN ITEMS / -)
        string(FIND "${composer_shared_build_options}" "${prefix}${option}" composer_option_position)

        if(NOT composer_option_position EQUAL -1)
            message(FATAL_ERROR "The build options include ${prefix}${option}, which can change the "
                                "instrument's floating-point results; the frozen v1 comparison cannot "
                                "detect it.")
        endif()
    endforeach()
endforeach()

# The synthesiser core, tested without JUCE.
set(composer_instrument_synth_test_sources
    synth/SynthTestMain.cpp
    synth/Spectrum.cpp
    synth/ParameterTests.cpp
    synth/DspTests.cpp
    synth/SynthTests.cpp
    synth/PatchStateOrderTests.cpp
    "${PROJECT_SOURCE_DIR}/tests/support/TestProcess.cpp")

add_executable(composer_instrument_synth_tests ${composer_instrument_synth_test_sources})
target_link_libraries(composer_instrument_synth_tests PRIVATE composer::instrument_synth Catch2::Catch2)
target_include_directories(composer_instrument_synth_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/instrument/synth"
        "${PROJECT_SOURCE_DIR}/tests/support")
composer_apply_warnings(composer_instrument_synth_tests ${composer_instrument_synth_test_sources})

catch_discover_tests(composer_instrument_synth_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 300)
