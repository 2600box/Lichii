// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace FrontendCommon::NxArchive {

/// The archive path to write: `path` with ".nx" appended unless it already ends with it.
std::string WithNxExtension(std::string path);

} // namespace FrontendCommon::NxArchive
