// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "common/common_types.h"
#include "video_core/shader_cache_v2/cache_writer.h"

namespace VideoCommon::ShaderCacheV2 {

struct ProgramRecord;

/// EDEN_NX2_SHADER_CACHE=v2, read once: the Vulkan pipeline cache keeps its records in a v2 log
/// instead of vulkan.fork-v<version>.bin. A test switch until v2 replaces the old cache.
[[nodiscard]] bool Enabled();

/// The log of a title, in its shader cache directory
inline constexpr std::string_view FileName{"lichii-cache.v2.bin"};
/// What the driver compiled for that title's pipelines, beside it (driver_blobs.h)
inline constexpr std::string_view DriverFileName{"lichii-cache.v2.driver.bin"};

/// A title's v2 log: the thread writing it, and the programs it holds.
class Cache {
public:
    explicit Cache(std::filesystem::path file, WriterOptions options = {});

    [[nodiscard]] CacheWriter& Writer() noexcept {
        return writer;
    }

    /// Notes a Program record the log holds (see ProgramStamp), so that it isn't written again
    void AddProgram(u64 key, u64 lowering_stamp);

    /// Queues the record of a program unless the log holds it from this build's lowering
    void RecordProgram(const ProgramRecord& record);

private:
    std::mutex mutex;
    std::unordered_map<u64, u64> programs; ///< Record key -> lowering stamp
    CacheWriter writer;
};

} // namespace VideoCommon::ShaderCacheV2
