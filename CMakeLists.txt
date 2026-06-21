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

# --- OBS dependency (optional) --------------------------------------------
# When DANCEHAP_WITH_OBS_DEPS is ON, we resolve the real libobs headers +
# import library. Two strategies:
#   1. OBS's own CMake config (libobsConfig.cmake) — preferred, produced by
#      `cmake --install --component Development` in CI.
#   2. Our cmake/FindLibObs.cmake — fallback for manual setups.
# When OFF (default for local dev) the plugin compiles in "stub" mode.
if(DANCEHAP_WITH_OBS_DEPS)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(libobs CONFIG QUIET)
    if(TARGET OBS::libobs)
        message(STATUS "OBS SDK: using libobsConfig.cmake (OBS::libobs)")
        set(DANCEHAP_OBS_LIB OBS::libobs)
    else()
        find_package(LibObs REQUIRED)
        message(STATUS "OBS SDK: using FindLibObs.cmake (LibObs::LibObs)")
        set(DANCEHAP_OBS_LIB LibObs::LibObs)
    endif()
endif()

# --- FFmpeg dependency (optional, for real HapDemuxer) ----------------------
# When DANCEHAP_WITH_OBS_DEPS is ON, obs-deps ships FFmpeg headers + libs.
# We try pkg-config first, then manual find. If found, the real demuxer
# implementation is compiled (DANCEHAP_HAVE_FFMPEG=1); otherwise the stub
# implementation is used (sufficient for unit tests — no crash, just
# synthetic data).
set(DANCEHAP_HAVE_FFMPEG 0)
if(DANCEHAP_WITH_OBS_DEPS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(DH_AVFORMAT QUIET IMPORTED_TARGET libavformat)
        pkg_check_modules(DH_AVCODEC  QUIET IMPORTED_TARGET libavcodec)
        pkg_check_modules(DH_AVUTIL   QUIET IMPORTED_TARGET libavutil)
    endif()
    if(TARGET PkgConfig::DH_AVFORMAT AND TARGET PkgConfig::DH_AVCODEC AND TARGET PkgConfig::DH_AVUTIL)
        set(DANCEHAP_HAVE_FFMPEG 1)
        set(DANCEHAP_FFMPEG_LIBS PkgConfig::DH_AVFORMAT PkgConfig::DH_AVCODEC PkgConfig::DH_AVUTIL)
        message(STATUS "FFmpeg: found via pkg-config")
    else()
        find_path(DH_FFMPEG_INCLUDE_DIR
            NAMES libavformat/avformat.h
            PATH_SUFFIXES include)
        find_library(DH_AVFORMAT_LIB NAMES avformat libavformat avformat-61)
        find_library(DH_AVCODEC_LIB  NAMES avcodec libavcodec avcodec-61)
        find_library(DH_AVUTIL_LIB   NAMES avutil libavutil avutil-59)
        if(DH_FFMPEG_INCLUDE_DIR AND DH_AVFORMAT_LIB AND DH_AVCODEC_LIB AND DH_AVUTIL_LIB)
            set(DANCEHAP_HAVE_FFMPEG 1)
            set(DANCEHAP_FFMPEG_LIBS ${DH_AVFORMAT_LIB} ${DH_AVCODEC_LIB} ${DH_AVUTIL_LIB})
            message(STATUS "FFmpeg: found via manual search (inc=${DH_FFMPEG_INCLUDE_DIR})")
        else()
            message(STATUS "FFmpeg: not found — using stub demuxer (real-OBS build)")
        endif()
    endif()
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
    src/hap_demuxer.cpp
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
    target_link_libraries(dancehap_objs PRIVATE ${DANCEHAP_OBS_LIB})
    target_compile_definitions(dancehap_objs PRIVATE DANCEHAP_HAVE_OBS=1)
endif()

# FFmpeg linking for real demuxer (only when WITH_OBS_DEPS + FFmpeg found).
if(DANCEHAP_HAVE_FFMPEG)
    target_compile_definitions(dancehap_objs PRIVATE DANCEHAP_HAVE_FFMPEG=1)
    if(DH_FFMPEG_INCLUDE_DIR)
        target_include_directories(dancehap_objs PRIVATE ${DH_FFMPEG_INCLUDE_DIR})
    endif()
    target_link_libraries(dancehap_objs PRIVATE ${DANCEHAP_FFMPEG_LIBS})
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
    target_link_libraries(dancehap PRIVATE ${DANCEHAP_OBS_LIB})
endif()

# FFmpeg libs must also be linked into the MODULE (OBJECT libraries do not
# propagate transitive link dependencies).
if(DANCEHAP_HAVE_FFMPEG)
    target_compile_definitions(dancehap PRIVATE DANCEHAP_HAVE_FFMPEG=1)
    target_link_libraries(dancehap PRIVATE ${DANCEHAP_FFMPEG_LIBS})
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
message(STATUS "DanceHAP ${PROJECT_VERSION} — Phase 1.2 hap demuxer")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Platform     : ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  OBS deps     : ${DANCEHAP_WITH_OBS_DEPS}")
message(STATUS "  FFmpeg       : ${DANCEHAP_HAVE_FFMPEG}")
message(STATUS "  Tests        : ${DANCEHAP_BUILD_TESTS}")
message(STATUS "=========================================================")
