# Applies the patches Kick needs to a fresh JUCE 7.0.12 checkout. Idempotent via a flag file.
#   cmake -DJUCE_DIR=<dir> -DKICK_ROOT=<repo> -DTESTS_UI=<tests/ui> -P apply_juce_patches.cmake
set(flag "${JUCE_DIR}/.kick_patches_applied")
if(EXISTS "${flag}")
    return()
endif()

set(patches "${KICK_ROOT}/Kick/patches/juce_multitouch_7.0.12.patch")
if(APPLE)
    # The macOS 15+ SDK removed CGWindowListCreateImage, which JUCE 7.0.12 still calls.
    list(APPEND patches "${TESTS_UI}/patches/juce_7.0.12_macos15_sdk.patch")
endif()

foreach(p IN LISTS patches)
    execute_process(COMMAND git apply --ignore-whitespace "${p}" WORKING_DIRECTORY "${JUCE_DIR}" RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(rc)
        # Fall back to patch(1) with fuzz, which tolerates context drift.
        execute_process(COMMAND patch -p1 -F3 -i "${p}" WORKING_DIRECTORY "${JUCE_DIR}" RESULT_VARIABLE rc2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2)
        if(rc2)
            if(p MATCHES "macos15_sdk")
                # Only needed on the newest SDKs; older ones still have the API.
                message(WARNING "Skipping ${p}: ${err} ${out2} ${err2}")
            else()
                message(FATAL_ERROR "Failed to apply ${p} to JUCE: ${err} ${out2} ${err2}")
            endif()
        endif()
    endif()
endforeach()
file(WRITE "${flag}" "patched")
