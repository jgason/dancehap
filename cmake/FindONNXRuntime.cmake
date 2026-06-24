# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# cmake/FindONNXRuntime.cmake
#
# Find ONNX Runtime C API headers and library.
#
# Defines:
#   ONNXRuntime_FOUND          - TRUE if found
#   ONNXRuntime_INCLUDE_DIRS   - path to onnxruntime_c_api.h
#   ONNXRuntime_LIBRARIES      - onnxruntime library
#   ONNXRuntime_VERSION        - version string (if detectable)
#
# Usage:
#   find_package(ONNXRuntime)
#   if(ONNXRuntime_FOUND)
#       target_link_libraries(my_target PRIVATE ${ONNXRuntime_LIBRARIES})
#       target_include_directories(my_target PRIVATE ${ONNXRuntime_INCLUDE_DIRS})
#   endif()
#
# Phase 2.1: module created, not yet invoked in main CMakeLists.
# Phase 2.2: will be invoked with DANCEHAP_HAVE_ONNXRUNTIME.

include(FindPackageHandleStandardArgs)

# --- Search paths ---
# Windows: NuGet package or manual install
# macOS: Homebrew or manual install
# Linux: system or manual install (non-goal but supported)

set(_ONNX_SEARCH_PATHS
    # Windows NuGet
    "${CMAKE_SOURCE_DIR}/packages/Microsoft.ML.OnnxRuntime.*/build/native"
    # Homebrew macOS
    /opt/homebrew/opt/onnxruntime
    /usr/local/opt/onnxruntime
    # Manual install
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime"
    # System
    /usr
    /usr/local
)

# --- Headers ---
find_path(ONNXRuntime_INCLUDE_DIRS
    NAMES onnxruntime_c_api.h
    HINTS ${_ONNX_SEARCH_PATHS}
    PATH_SUFFIXES include include/onnxruntime
    DOC "ONNX Runtime include directory"
)

# --- Library ---
if(WIN32)
    set(_ONNX_LIB_NAMES onnxruntime)
elseif(APPLE)
    set(_ONNX_LIB_NAMES onnxruntime libonnxruntime)
else()
    set(_ONNX_LIB_NAMES onnxruntime libonnxruntime)
endif()

find_library(ONNXRuntime_LIBRARIES
    NAMES ${_ONNX_LIB_NAMES}
    HINTS ${_ONNX_SEARCH_PATHS}
    PATH_SUFFIXES lib lib/x64
    DOC "ONNX Runtime library"
)

# --- Version (best-effort) ---
if(ONNXRuntime_INCLUDE_DIRS)
    set(_ONNX_VER_FILE "${ONNXRuntime_INCLUDE_DIRS}/onnxruntime_c_api.h")
    if(EXISTS "${_ONNX_VER_FILE}")
        file(STRINGS "${_ONNX_VER_FILE}" _ONNX_VER_LINES
            REGEX "^#define ORT_API_VERSION[ \t]+[0-9]+")
        if(_ONNX_VER_LINES)
            string(REGEX MATCH "[0-9]+" ONNXRuntime_VERSION "${_ONNX_VER_LINES}")
        endif()
    endif()
endif()

find_package_handle_standard_args(ONNXRuntime
    REQUIRED_VARS ONNXRuntime_INCLUDE_DIRS ONNXRuntime_LIBRARIES
    VERSION_VAR ONNXRuntime_VERSION
)

mark_as_advanced(ONNXRuntime_INCLUDE_DIRS ONNXRuntime_LIBRARIES ONNXRuntime_VERSION)