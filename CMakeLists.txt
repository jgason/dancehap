# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# DanceHAP — Root CMake build
# Phase 1.0 skeleton: OBS plugin MODULE + GoogleTest unit tests.
# Builds on Windows (MSVC) and macOS (Apple Clang). Linux is a non-goal
# (ADR-001) but the skeleton will still configure on Linux for local dev.
#
# See docs/plans/PLAN-PHASE-1.md, étape 1.0.

cmake_minimum_required(VERSION 3.22)

project(DanceHAP
    VERSION 0.1.0
    DESCRIPTION "OBS plugin for immersive dance scenes: HAP clips + real-time matting + alpha overlays"
    LANGUAGES CXX
)

# --- C++ standard -------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position-independent code (required for MODULE on most platforms)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- Build options ------------------------------------------------------
option(DANCEHAP_BUILD_TESTS       "Build GoogleTest unit tests"             ON)
option(DANCEHAP_WITH_OBS_DEPS     "Link against a real libobs installation" OFF)
option(DANCEHAP_INSTALL           "Generate install targets"                OFF)

# --- Version header -----------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/dancehap/version.h"
    @ONLY
)

# --- OBS dependency (optional for skeleton) -----------------------------
# When DANCEHAP_WITH_OBS_DEPS is ON, cmake/FindLibObs.cmake resolves the
# real libobs headers + import library.  When OFF (the default for the
# Phase 1.0 skeleton) the plugin compiles in "stub" mode: it defines the
# OBS module entry points directly without including <obs-module.h>.
# Phase 1.1 will flip the default to ON once CI ships prebuilt OBS deps.
if(DANCEHAP_WITH_OBS_DEPS)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(LibObs REQUIRED)
endif()

# --- Plugin object library (shared between MODULE and unit tests) -------
# We compile plugin + hap_clip_source once into an OBJECT library so both
# the MODULE target (the actual OBS plugin .dll/.so) and the unit-test
# executable can consume it without duplicate compilation.
#
# When DANCEHAP_WITH_OBS_DEPS is OFF, we also compile obs_stub.cpp which
# provides functional stub implementations of the OBS API so the code
# compiles and tests run without a libobs installation.
set(DANCEHAP_SOURCES
    src/plugin.cpp
    src/hap_clip_source.cpp
)
if(NOT DANCEHAP_WITH_OBS_DEPS)
    list(APPEND DANCEHAP_SOURCES src/obs_stub.cpp)
endif()

add_library(dancehap_objs OBJECT ${DANCEHAP_SOURCES})

target_include_directories(dancehap_objs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_features(dancehap_objs PRIVATE cxx_std_17)

if(DANCEHAP_WITH_OBS_DEPS)
    target_link_libraries(dancehap_objs PRIVATE LibObs::LibObs)
    target_compile_definitions(dancehap_objs PRIVATE DANCEHAP_HAVE_OBS=1)
endif()

# --- Plugin target ------------------------------------------------------
add_library(dancehap MODULE $<TARGET_OBJECTS:dancehap_objs>)

target_include_directories(dancehap PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_features(dancehap PRIVATE cxx_std_17)

# Propagate OBS definitions to the MODULE link step.
if(DANCEHAP_WITH_OBS_DEPS)
    target_link_libraries(dancehap PRIVATE LibObs::LibObs)
endif()

# OBS plugins use no "lib" prefix and a platform-specific extension.
set_target_properties(dancehap PROPERTIES PREFIX "")

if(WIN32)
    set_target_properties(dancehap PROPERTIES SUFFIX ".dll")
elseif(APPLE)
    # OBS loads macOS plugins as .so (not .dylib)
    set_target_properties(dancehap PROPERTIES SUFFIX ".so")
else()
    set_target_properties(dancehap PROPERTIES SUFFIX ".so")
endif()

# --- Compiler warnings --------------------------------------------------
if(MSVC)
    target_compile_options(dancehap PRIVATE /W4 /permissive-)
else()
    target_compile_options(dancehap PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- Tests --------------------------------------------------------------
if(DANCEHAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Install rules (placeholder for Phase 5 packaging) ------------------
if(DANCEHAP_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS dancehap
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
        RUNTIME DESTINATION "obs-plugins/64bit"
    )
endif()

# --- Status summary -----------------------------------------------------
message(STATUS "=========================================================")
message(STATUS "DanceHAP ${PROJECT_VERSION} — Phase 1.1 hap_clip_source")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Platform     : ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  OBS deps     : ${DANCEHAP_WITH_OBS_DEPS}")
message(STATUS "  Tests        : ${DANCEHAP_BUILD_TESTS}")
message(STATUS "=========================================================")
