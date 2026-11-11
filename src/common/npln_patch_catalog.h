// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// NPLN titles pin Nintendo's certificate, so each exact title build needs a small signed patch
// from nextendo.network before it can reach Nextendo. Ported from citron-nextendo's
// src/web_service/npln_patch_catalog.cpp.
namespace Common::NplnPatchCatalog {

struct Lookup {
    // Set when the launch has a signed entry for the module; ips stays empty when that entry is
    // withdrawn or its guards do not match, and nothing may be applied in its place.
    bool authoritative{};
    std::string id;
    u64 version{};
    std::vector<u8> ips;
};

// Runs on a frontend worker before the game loads, never in the loader. While Nextendo is on and
// the signed title index lists the title, reads the main build ID, refreshes that build's patch
// at most every 15 minutes and fixes the patches this launch sees.
void PrepareForGame(u64 title_id, const std::function<std::optional<std::string>()>& read_build_id);

// For the loader: no network access, and nothing while Nextendo is off.
bool HasEntry(u64 title_id, std::string_view build_id, std::string_view module);
Lookup Find(u64 title_id, std::string_view build_id, std::string_view module,
            std::span<const u8> image);

} // namespace Common::NplnPatchCatalog
