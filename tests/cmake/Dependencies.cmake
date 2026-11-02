include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# Allow offline / pre-seeded builds: -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(googletest)
