# SPDX-License-Identifier: AGPL-3.0-or-later
include(FetchContent)

# Same pin as the repo's other gtest tree, so the two can merge later.
# Pass -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<path> to build offline.
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

get_filename_component(KICK_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_executable(kick_mcu_tests
    alloc_hook.cpp
    frame_codec_test.cpp
    rx_parser_test.cpp
    tx_queue_test.cpp
    session_test.cpp
    link_test.cpp
    storm_test.cpp
    trace_replay_test.cpp
    no_alloc_test.cpp
)
target_link_libraries(kick_mcu_tests PRIVATE kick_mcu_bsp_sim GTest::gtest_main)
target_compile_options(kick_mcu_tests PRIVATE ${KICK_MCU_WARNINGS})
target_compile_definitions(kick_mcu_tests PRIVATE
    KICK_TRACE_DIR="${KICK_REPO_ROOT}/emu/tests/traces"
)

# The contract checks compile against the real host headers. dcl_contracts.h pulls in JUCE only
# for a type it does not use, so a stub juce_core.h keeps JUCE out of the build.
add_executable(kick_mcu_contract_tests
    contract_test.cpp
    pktmgr_crosscheck_test.cpp
    ${KICK_REPO_ROOT}/ComProtocol/PktMgr/PktMgr.cpp
)
target_include_directories(kick_mcu_contract_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${KICK_REPO_ROOT}/Kick/modules/contracts/dcl
    ${KICK_REPO_ROOT}/ComProtocol/PktMgr
    ${KICK_REPO_ROOT}/Kick/modules/input_events
)
target_link_libraries(kick_mcu_contract_tests PRIVATE kick_mcu_bsp_sim GTest::gtest_main)
# PktMgr.cpp is host code, not ours: keep its warnings out of the log.
set_source_files_properties(${KICK_REPO_ROOT}/ComProtocol/PktMgr/PktMgr.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(kick_mcu_contract_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter>)

include(GoogleTest)
gtest_discover_tests(kick_mcu_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(kick_mcu_contract_tests DISCOVERY_MODE PRE_TEST)
