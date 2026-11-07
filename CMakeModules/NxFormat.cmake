# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# nxformat (https://github.com/fab918/nxformat) reads and writes .nx game
# archives. The core uses its header-only C++ reader; the frontends call its
# C library to convert games to .nx and to edit them. The pinned release is
# downloaded and checked here. LICHII_NXFORMAT_ROOT may instead point at an
# unpacked release, such as a local build packed by nxformat's
# tools/package_release.py.
#
# Defines nxformat::reader, nxformat::nxformat and LICHII_NXFORMAT_RUNTIME,
# the shared library to ship next to the executable.
set(LICHII_NXFORMAT_ROOT "" CACHE PATH "Unpacked nxformat release (empty: download the pinned one)")

set(_nxformat_version 1.0.0)
set(_nxformat_tag V1.0.0)
set(_nxformat_root "${LICHII_NXFORMAT_ROOT}")
if (NOT _nxformat_root)
    if (ANDROID)
        set(_nxformat_platform android-arm64)
        set(_nxformat_sha256 bc12f2430481444bc59335f667d399fbbd94ce8eb242ee027277529530764caf)
    elseif (WIN32 AND ARCHITECTURE_x86_64)
        set(_nxformat_platform windows-x64)
        set(_nxformat_sha256 c71a65d2f2cf16c8a671850548a88e67e1112cd062eb6566faa2bd7c7979a881)
    elseif (APPLE)
        set(_nxformat_platform macos-universal)
        set(_nxformat_sha256 a90787e798bb11a3bf334db6976b9ba88a8a09f0a1b6733fd9511dcf9b215d78)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARCHITECTURE_x86_64)
        set(_nxformat_platform linux-x64)
        set(_nxformat_sha256 d12f7e3fd0cfe2c2a30577dbd83146d0ebfd601cc7da400e870d50fbc4272aad)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARCHITECTURE_arm64)
        set(_nxformat_platform linux-arm64)
        set(_nxformat_sha256 89c2546fefe48f7ea5a7cee252cb345a90ec8e1d7b2e1c714abfc4afa25e0919)
    else()
        message(FATAL_ERROR "No nxformat build for this platform: set LICHII_NXFORMAT_ROOT")
    endif()
    if (WIN32)
        set(_nxformat_ext zip)
    else()
        set(_nxformat_ext tar.gz)
    endif()
    set(_nxformat_name "nxformat-${_nxformat_version}-${_nxformat_platform}")
    set(_nxformat_archive "${CMAKE_BINARY_DIR}/externals/${_nxformat_name}.${_nxformat_ext}")
    set(_nxformat_root "${CMAKE_BINARY_DIR}/externals/${_nxformat_name}")
    if (NOT EXISTS "${_nxformat_root}/lib/cmake/nxformat/nxformatConfig.cmake")
        file(DOWNLOAD
            "https://github.com/fab918/nxformat/releases/download/${_nxformat_tag}/${_nxformat_name}.${_nxformat_ext}"
            "${_nxformat_archive}"
            EXPECTED_HASH SHA256=${_nxformat_sha256}
            TLS_VERIFY ON
            STATUS _nxformat_status)
        list(GET _nxformat_status 0 _nxformat_code)
        if (NOT _nxformat_code EQUAL 0)
            list(GET _nxformat_status 1 _nxformat_message)
            message(FATAL_ERROR "Could not download ${_nxformat_name}: ${_nxformat_message}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_nxformat_archive}" DESTINATION "${_nxformat_root}")
    endif()
endif()

find_package(nxformat ${_nxformat_version} REQUIRED CONFIG
    PATHS "${_nxformat_root}/lib/cmake/nxformat" NO_DEFAULT_PATH)
foreach (_nxformat_target nxformat::nxformat nxformat::reader)
    if (TARGET ${_nxformat_target})
        set_target_properties(${_nxformat_target} PROPERTIES IMPORTED_GLOBAL TRUE)
    endif()
endforeach()
set(LICHII_NXFORMAT_RUNTIME "${NXFORMAT_RUNTIME}" CACHE INTERNAL "")
message(STATUS "nxformat ${nxformat_VERSION}: ${_nxformat_root}")

foreach (_nxformat_var version tag root platform sha256 ext name archive status code message target)
    unset(_nxformat_${_nxformat_var})
endforeach()
