// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/shader_cache_v2/cache_format.h"

namespace Vulkan {

struct GraphicsPipelineCacheKey;

/// Clears the driver of a graphics key, which the v2 cache keeps out of its keys: a driver update
/// would leave every recorded pipeline unmatched.
void ClearDriverFields(GraphicsPipelineCacheKey& key);

/// The v2 record key of a compute pipeline record (SerializePipelinePayload): a hash of all of it.
/// The translations rebuilt for one pipeline key stay beside each other, as in the old cache.
[[nodiscard]] u64 ComputeRecordKey(std::span<const u8> payload);

/// The v2 record key of a graphics pipeline record: its key's hash, driver cleared
[[nodiscard]] u64 GraphicsRecordKey(const GraphicsPipelineCacheKey& key);

struct LegacyCacheMigration {
    std::optional<u32> version; ///< The cache version migrated from
    u32 graphics{};
    u32 compute{};
    u32 collapsed{}; ///< Records a later one of the same record key replaced
    u32 used{};      ///< Used-list entries carried over
    bool failed{};   ///< There was a cache to migrate, but the log couldn't be written
};

/// Starts the v2 `log` from the newest Vulkan disk cache in `directory` (vulkan.fork-v<version>,
/// the current version first, then the compatible ones) and the used list beside it, unless the log
/// exists. Graphics keys lose their driver fields; records sharing a record key collapse into the
/// last one. A Migrated record names the source. The older files are only read.
LegacyCacheMigration MigrateLegacyPipelineCache(const std::filesystem::path& directory,
                                                const std::filesystem::path& log, u8 max_used_age);

/// The used-list session key and time of a used list carried over: `age` sessions old
[[nodiscard]] u64 MigratedUsedSessionKey(u8 age);
[[nodiscard]] u64 MigratedUsedSessionTime(u8 age);

/// Graphics key hashes the newest `max_age` sessions of the log's Used records used, with how many
/// sessions ago each was last used (as the old used list holds them). `expired` gets the record
/// keys of older sessions.
[[nodiscard]] std::unordered_map<u64, u8> UsedPipelinesFromLog(
    std::span<const VideoCommon::ShaderCacheV2::LogRecord> live, u8 max_age,
    std::vector<u64>& expired);

} // namespace Vulkan
