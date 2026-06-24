# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# tests/CMakeLists.txt — GoogleTest 1.14 unit test harness for DanceHAP.
#
# FetchContent pulls GoogleTest v1.14.0 (BSD-3, compatible with our MIT
# licence per ADR-005). Tests are standalone executables; they do NOT
# require a running OBS instance.

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

# On Windows, force the shared CRT so MSVC runtimes don't conflict.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# --- Unit test executable ------------------------------------------------
add_executable(dancehap_unit_tests
    unit/test_skeleton.cpp
    unit/test_hap_clip_source.cpp
    unit/test_hap_demuxer.cpp
    unit/test_hap_decoder.cpp
    unit/test_clip_player.cpp
    unit/test_ai_matte_filter.cpp
    unit/test_matte_engine.cpp
)

target_link_libraries(dancehap_unit_tests PRIVATE
    dancehap_objs
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(dancehap_unit_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
)

# Absolute path to the test HAP asset (used by demuxer integration tests).
target_compile_definitions(dancehap_unit_tests PRIVATE
    DANCEHAP_TEST_ASSET="${CMAKE_SOURCE_DIR}/tests/assets/sample_hapa_5s.mov"
)

target_compile_features(dancehap_unit_tests PRIVATE cxx_std_17)

# Auto-discover tests for ctest.
# On Windows, the default 5-second timeout for test discovery is too short:
# the .exe takes longer to start (DLL loading + Defender scan). Bump to 60s.
# NOTE: do NOT use DISCOVERY_MODE PRE_TEST — it has a known bug with Visual
# Studio multi-config generators (the per-config include file gets a literal
# '-' suffix instead of the config name, causing 'include could not find
# requested file' at ctest time). POST_BUILD (default) works correctly.
include(GoogleTest)
gtest_discover_tests(dancehap_unit_tests
    DISCOVERY_TIMEOUT 60
    PROPERTIES TIMEOUT 30)
