// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <string_view>

#include "video_core/shader_cache_v2/cache.h"
#include "video_core/shader_cache_v2/records.h"

namespace VideoCommon::ShaderCacheV2 {

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_SHADER_CACHE");
        return value != nullptr && std::string_view{value} == "v2";
    }();
    return enabled;
}

Cache::Cache(std::filesystem::path file, WriterOptions options)
    : writer{std::move(file), options} {}

void Cache::AddProgram(u64 key, u64 lowering_stamp) {
    std::scoped_lock lock{mutex};
    programs.insert_or_assign(key, lowering_stamp);
}

void Cache::RecordProgram(const ProgramRecord& record) {
    const u64 key = ProgramKey(record);
    {
        std::scoped_lock lock{mutex};
        const auto [it, inserted] = programs.try_emplace(key, record.lowering_stamp);
        if (!inserted) {
            if (it->second == record.lowering_stamp) {
                return;
            }
            it->second = record.lowering_stamp;
        }
    }
    writer.Append(RecordKind::Program, key, EncodeProgram(record));
}

} // namespace VideoCommon::ShaderCacheV2
