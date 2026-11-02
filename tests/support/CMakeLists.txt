# Product code under test, compiled without JUCE.

set(AUDIO_ROOT ${KICK_REPO_ROOT}/AudIO/AudIO)

add_library(kick_pktmgr STATIC ${KICK_REPO_ROOT}/ComProtocol/PktMgr/PktMgr.cpp)
target_include_directories(kick_pktmgr PUBLIC ${KICK_REPO_ROOT}/ComProtocol/PktMgr)

add_library(kick_audio_core OBJECT audio_unity.cpp)
target_include_directories(kick_audio_core PUBLIC
    ${AUDIO_ROOT}/Includes ${AUDIO_ROOT}/3rdparty ${AUDIO_ROOT}/Objects ${AUDIO_ROOT}/Dsp ${AUDIO_ROOT}/Core ${AUDIO_ROOT})
# Product sources carry their own warnings; keep the test log readable.
target_compile_options(kick_audio_core PRIVATE -w)
target_compile_options(kick_pktmgr PRIVATE -w)

add_library(kick_test_support INTERFACE)
target_include_directories(kick_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
