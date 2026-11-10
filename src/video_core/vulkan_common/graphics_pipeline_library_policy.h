// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

// One descriptor set per graphics stage, including holes for absent stages.
constexpr u32 GraphicsPipelineLibraryDescriptorSetCount = 5;

constexpr bool CanUseGraphicsPipelineLibraries(bool feature, bool fast_linking, u32 max_sets) {
    return feature && fast_linking && max_sets >= GraphicsPipelineLibraryDescriptorSetCount;
}

} // namespace Vulkan
