# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# FindLibObs.cmake — resolve a prebuilt libobs installation.
#
# Phase 1.0 skeleton: this module is NOT exercised by default
# (DANCEHAP_WITH_OBS_DEPS=OFF). It exists so that Phase 1.1 can flip
# the switch and link against real OBS headers/libraries, either from
# a system install or from the prebuilt dependency packages published
# at https://github.com/obsproject/obs-deps/releases.
#
# Usage:
#   find_package(LibObs REQUIRED)
#   target_link_libraries(my_target PRIVATE LibObs::LibObs)
#
# Variables honoured:
#   LIBOBS_INCLUDE_DIR  — path to libobs headers (obs.h lives here)
#   LIBOBS_LIBRARY      — path to libobs import/static library
#
# Created target:
#   LibObs::LibObs      — INTERFACE imported target with include + lib

# 1. Try the hint variables first.
find_path(LIBOBS_INCLUDE_DIR
    NAMES obs.h
    PATHS
        "${LIBOBS_INCLUDE_DIR}"
        "${LIBOBS_DIR}/include"
        "${LIBOBS_DIR}/libobs"
    DOC "Path to the libobs include directory (contains obs.h)"
)

find_library(LIBOBS_LIBRARY
    NAMES obs libobs
    PATHS
        "${LIBOBS_LIBRARY}"
        "${LIBOBS_DIR}/lib"
        "${LIBOBS_DIR}"
    DOC "Path to the libobs library"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibObs
    REQUIRED_VARS LIBOBS_INCLUDE_DIR LIBOBS_LIBRARY
)

if(LibObs_FOUND)
    add_library(LibObs::LibObs INTERFACE IMPORTED)
    set_target_properties(LibObs::LibObs PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES      "${LIBOBS_LIBRARY}"
    )
    message(STATUS "LibObs found:")
    message(STATUS "  includes : ${LIBOBS_INCLUDE_DIR}")
    message(STATUS "  library  : ${LIBOBS_LIBRARY}")
endif()

mark_as_advanced(LIBOBS_INCLUDE_DIR LIBOBS_LIBRARY)
