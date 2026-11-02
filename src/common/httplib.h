// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Exporting Keychain roots into OpenSSL loses their trust restrictions. macOS
// uses the pinned dependency's native SecTrust verifier on every TLS connection.
#define CPPHTTPLIB_DISABLE_MACOSX_AUTOMATIC_ROOT_CERTIFICATES 1
#define CPPHTTPLIB_OPENSSL_SUPPORT 1

#ifdef __GNUC__
#pragma GCC diagnostic push
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif
#include <httplib.h>
#if defined(__APPLE__) && TARGET_OS_OSX && !defined(CPPHTTPLIB_EGLANTINE_MACOS_NATIVE_TRUST)
#error "macOS requires Eglantine's patched cpp-httplib native trust verifier"
#endif
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
