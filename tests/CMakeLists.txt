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

target_compile_features(dancehap_unit_tests PRIVATE cxx_std_17)

# Auto-discover tests for ctest.
include(GoogleTest)
gtest_discover_tests(dancehap_unit_tests)
