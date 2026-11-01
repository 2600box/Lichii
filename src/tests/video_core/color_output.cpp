// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/vk_color_output.h"

TEST_CASE("MoltenVK output types include disabled render targets", "[video_core][vulkan]") {
    using Format = Tegra::RenderTargetFormat;
    using Type = Shader::AttributeType;
    const auto type = [](Format format) { return Vulkan::ColorOutputType(static_cast<u8>(format)); };
    // This must not enter the non-constexpr pixel-format converter/logging path.
    STATIC_REQUIRE(Vulkan::ColorOutputType(static_cast<u8>(Format::NONE)) == Type::Float);
    for (const auto format : {Format::A8B8G8R8_UNORM, Format::A8B8G8R8_SRGB,
                              Format::R16G16B16A16_FLOAT, Format::R32_FLOAT}) {
        REQUIRE(type(format) == Type::Float);
    }
    for (const auto format : {Format::R8_SINT, Format::R16G16_SINT, Format::R32G32B32A32_SINT}) {
        REQUIRE(type(format) == Type::SignedInt);
    }
    for (const auto format : {Format::R8_UINT, Format::R16G16_UINT, Format::R32G32B32A32_UINT}) {
        REQUIRE(type(format) == Type::UnsignedInt);
    }
    const std::array<Format, 8> attachments{Format::R32_UINT};
    REQUIRE(type(attachments[0]) == Type::UnsignedInt);
    for (size_t slot = 1; slot < attachments.size(); ++slot) {
        REQUIRE(type(attachments[slot]) == Type::Float);
    }
}
