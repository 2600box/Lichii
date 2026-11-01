// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/nx2_video_decoder.h"

namespace {

constexpr std::array<u8, 10> Configuration{'v', 'i', 'd', 'e', 'o', '/', 'a', 'v', '1', 0};
constexpr std::array<u8, 36> RedFrame{
    0x12, 0x00, 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x9f, 0xf9, 0xb5, 0xf3,
    0x00, 0x80, 0x32, 0x14, 0x10, 0x00, 0x80, 0x00, 0x00, 0x14, 0xb9, 0xb9,
    0xac, 0x6e, 0x4d, 0x14, 0x5e, 0x91, 0x36, 0xba, 0xe9, 0xbc, 0x64, 0xa0,
};

} // namespace

TEST_CASE("AV1 support preserves NVDEC codec mappings", "[nx2_video][av1]") {
    using Tegra::Host1x::NvdecCommon::VideoCodec;
    for (const auto [hardware, software] :
         {std::pair{VideoCodec::H264, AV_CODEC_ID_H264},
          std::pair{VideoCodec::VP8, AV_CODEC_ID_VP8},
          std::pair{VideoCodec::VP9, AV_CODEC_ID_VP9}}) {
        FFmpeg::Decoder decoder{hardware};
        REQUIRE(decoder.GetCodec());
        CHECK(decoder.GetCodec()->id == software);
    }
}

TEST_CASE("Nx2 AV1 configuration selects a software codec", "[nx2_video][av1]") {
    for (const size_t length : {Configuration.size() - 1, Configuration.size()}) {
        const auto parsed = Tegra::Host1x::ParseNx2VideoConfiguration(
            std::span{Configuration}.first(length));
        REQUIRE(parsed);
        CHECK(parsed->codec == AV_CODEC_ID_AV1);
        CHECK(parsed->extradata.empty());
    }
    FFmpeg::Decoder decoder{AV_CODEC_ID_AV1};
    REQUIRE(decoder.GetCodec());
    CHECK(std::string_view{decoder.GetCodec()->name} == "libdav1d");
}

TEST_CASE("Nx2 AV1 decodes pixels and preserves timestamps across drain and reset",
          "[nx2_video][av1]") {
    using namespace Tegra::Host1x;
    const auto original = Settings::values.nvdec_emulation.GetValue();
    SCOPE_EXIT { Settings::values.nvdec_emulation.SetValue(original); };
    for (const auto mode : {Settings::NvdecEmulation::Cpu, Settings::NvdecEmulation::Gpu}) {
        Settings::values.nvdec_emulation.SetValue(mode);
        Nx2VideoDecoder decoder{Configuration};
        REQUIRE(decoder.IsOpen());
        std::vector<Nx2Picture> pictures;
        decoder.Decode(RedFrame, 100, pictures);
        decoder.Decode(RedFrame, 200, pictures);
        decoder.Drain(pictures);
        REQUIRE(pictures.size() == 2);
        CHECK(pictures[0].pts == 100);
        CHECK(pictures[1].pts == 200);
        for (const auto& picture : pictures) {
            CHECK(picture.width == 16);
            CHECK(picture.height == 16);
            CHECK_FALSE(picture.frame->IsHardwareDecoded());
            std::array<u8, 16 * 16 * 4> rgba{};
            decoder.Render(picture, Nx2PictureFormat::Rgba8, 16, 16, 64, 0, rgba);
            for (size_t i = 0; i < rgba.size(); i += 4) {
                CHECK(rgba[i] >= 250);
                CHECK(rgba[i + 1] <= 3);
                CHECK(rgba[i + 2] <= 3);
                CHECK(rgba[i + 3] == 255);
            }
            std::array<u8, 16 * 16 * 3 / 2> nv12{};
            decoder.Render(picture, Nx2PictureFormat::Nv12, 16, 16, 16, 256, nv12);
            CHECK(nv12[0] == 81);
            CHECK(nv12[256] == 90);
            CHECK(nv12[257] == 240);
        }
        pictures.clear();
        decoder.Drain(pictures);
        CHECK(pictures.empty());
        decoder.Decode(RedFrame, 300, pictures);
        decoder.Drain(pictures);
        REQUIRE(pictures.size() == 1);
        CHECK(pictures[0].pts == 300);
        pictures.clear();
        decoder.Decode(RedFrame, 400, pictures);
        decoder.Reset();
        pictures.clear();
        decoder.Decode(RedFrame, 500, pictures);
        decoder.Drain(pictures);
        REQUIRE(pictures.size() == 1);
        CHECK(pictures[0].pts == 500);
    }
}
