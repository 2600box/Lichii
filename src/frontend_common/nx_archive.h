// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace FrontendCommon::NxArchive {

/// The archive path to write: `path` with ".nx" appended unless it already ends with it.
std::string WithNxExtension(std::string path);

/// The regular files in `directory`, empty when it cannot be read. Never throws.
std::vector<std::filesystem::path> RegularFiles(const std::filesystem::path& directory);

} // namespace FrontendCommon::NxArchive
