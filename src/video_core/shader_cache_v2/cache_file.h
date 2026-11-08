// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/shader_cache_v2/cache_format.h"

namespace VideoCommon::ShaderCacheV2 {

/// A log read into memory; `parsed` points into `bytes`.
struct LogFile {
    std::vector<u8> bytes;
    ParsedLog parsed;

    LogFile() = default;
    LogFile(LogFile&&) = default;
    LogFile& operator=(LogFile&&) = default;
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
};

/// Reads the whole log without its lock: records are only appended under it, and the reader
/// ignores one cut off by an append still in progress. A missing file reads as empty.
[[nodiscard]] LogFile ReadLogFile(const std::filesystem::path& file);

enum class WriteResult {
    Written,
    Busy,         ///< Another process holds the log's lock
    Failed,       ///< A write failed; the file is as it was
    OtherVersion, ///< The file is another format version's, and stays as it is
    Exists,       ///< CreateLog found the file there already
};

/// Appends encoded records (EncodeRecord) holding the log's lock, which is tried once. The file
/// and its header are created when missing, a damaged header is written again, and a record cut
/// off by a crash is padded to the next record boundary first.
[[nodiscard]] WriteResult AppendToLog(const std::filesystem::path& file,
                                      std::span<const u8> records);

/// Rewrites the log with only its live records: to a temporary file that is committed to the disk
/// and renamed over it. Holding the log's lock, tried once, so that it never runs while another
/// process reads it under the lock or writes it.
[[nodiscard]] WriteResult CompactLog(const std::filesystem::path& file);

/// Creates the log with these encoded records the same way, unless it exists.
[[nodiscard]] WriteResult CreateLog(const std::filesystem::path& file,
                                    std::span<const u8> records);

[[nodiscard]] u64 UnixNow();

} // namespace VideoCommon::ShaderCacheV2
