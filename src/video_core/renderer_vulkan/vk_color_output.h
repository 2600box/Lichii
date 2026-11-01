// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shader_recompiler/runtime_info.h"
#include "video_core/surface.h"

namespace Vulkan {

constexpr Shader::AttributeType ColorOutputType(u8 render_target_format) {
    const auto format = static_cast<Tegra::RenderTargetFormat>(render_target_format);
    // Disabled attachments have no pixel format. In particular, do not feed None
    // to the format converter for every unused slot in every MoltenVK pipeline.
    if (format == Tegra::RenderTargetFormat::NONE) {
        return Shader::AttributeType::Float;
    }
    const auto pixel_format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
    if (!VideoCore::Surface::IsPixelFormatInteger(pixel_format)) {
        return Shader::AttributeType::Float;
    }
    return VideoCore::Surface::IsPixelFormatSignedInteger(pixel_format)
               ? Shader::AttributeType::SignedInt
               : Shader::AttributeType::UnsignedInt;
}

} // namespace Vulkan
