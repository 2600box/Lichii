// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Use the macOS Keychain trust store. A packaged app cannot rely on the
// Homebrew OpenSSL certificate directory from the machine that built it.
#define CPPHTTPLIB_OPENSSL_SUPPORT 1

#ifdef __GNUC__
#pragma GCC diagnostic push
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif
#include <httplib.h>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
