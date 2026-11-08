// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <string_view>

#include "frontend_common/nx_archive.h"

namespace FrontendCommon::NxArchive {

std::string WithNxExtension(std::string path) {
    constexpr std::string_view extension = ".nx";
    const bool has_extension =
        path.size() >= extension.size() &&
        std::equal(extension.begin(), extension.end(), path.end() - extension.size(),
                   [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
    if (!has_extension) {
        path += extension;
    }
    return path;
}

} // namespace FrontendCommon::NxArchive
