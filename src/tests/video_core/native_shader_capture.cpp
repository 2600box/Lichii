// SPDX-License-Identifier: GPL-3.0-or-later

#include <sstream>
#include <catch2/catch_test_macros.hpp>

#include "video_core/native_shader_capture.h"

namespace {
namespace Capture = VideoCommon::NativeShaderCapture;

TEST_CASE("Native shader capture bounds unique records", "[video_core][native_capture]") {
    Capture::Budget budget;
    for (u64 i = 0; i < Capture::MaxRecords; ++i) {
        REQUIRE(budget.Reserve(i));
        REQUIRE_FALSE(budget.Reserve(i));
    }
    REQUIRE_FALSE(budget.Reserve(100));
}

TEST_CASE("Native shader capture associates code and buffers in little endian records",
          "[video_core][native_capture]") {
    std::array<u8, 128> header{};
    std::array<u8, 16> code{};
    std::array<Capture::Buffer, Capture::MaxBuffers> buffers{};
    header[0] = 0x12;
    code[0] = 0x70;
    code[1] = 0x73;
    buffers[1] = {1, 0x123456789abcdef0ULL, 4, {1, 2, 3, 4}};
    std::ostringstream stream(std::ios::binary);
    REQUIRE(Capture::Write(stream, Capture::Title, 0x1122334455667788ULL, 42, 5, 0,
                           header, code, buffers));
    const auto bytes = stream.str();
    REQUIRE(bytes.size() == 56 + 128 + 16 + 18 * 24 + 4);
    REQUIRE(bytes.substr(0, 8) == "EGLACAP1");
    REQUIRE(static_cast<u8>(bytes[16]) == 0x88);
    REQUIRE(static_cast<u8>(bytes[23]) == 0x11);
    REQUIRE(static_cast<u8>(bytes[32]) == 4);
    REQUIRE(static_cast<u8>(bytes[56]) == 0x12);
    REQUIRE(static_cast<u8>(bytes[184]) == 0x70);
    REQUIRE(static_cast<u8>(bytes[224]) == 1);
    REQUIRE(static_cast<u8>(bytes[228]) == 1);
    REQUIRE(static_cast<u8>(bytes[232]) == 0xf0);
    REQUIRE(static_cast<u8>(bytes[248]) == 1);
    REQUIRE(static_cast<u8>(bytes[251]) == 4);
}

TEST_CASE("Native shader capture rejects invalid or oversized records before writing",
          "[video_core][native_capture]") {
    std::array<u8, 128> header{};
    std::vector<u8> code(16);
    std::array<Capture::Buffer, Capture::MaxBuffers> buffers{};
    std::ostringstream stream(std::ios::binary);
    REQUIRE_FALSE(Capture::Write(stream, 0, 0, 0, 0, 0, header, code, buffers));
    REQUIRE_FALSE(Capture::Write(stream, Capture::Title, 0, 0, 0, 0, {}, code, buffers));
    REQUIRE_FALSE(Capture::Write(stream, Capture::Title, 0, 0, 0, 16, header, code, buffers));
    REQUIRE_FALSE(Capture::Write(stream, Capture::Title, 0, 0, 0, 1, header, code, buffers));
    REQUIRE_FALSE(Capture::Write(stream, Capture::Title, 0, 0, 0, 0, header, code, {}));
    code.resize(Capture::MaxNativeBytes + 16);
    REQUIRE_FALSE(Capture::Write(stream, Capture::Title, 0, 0, 0, 0, header, code, buffers));
    code.resize(16);
    buffers[0] = {1, 0, 8, {1, 2, 3, 4}};
    REQUIRE_FALSE(Capture::Write(stream, Capture::Title, 0, 0, 0, 0, header, code, buffers));
    buffers[0].status = 2;
    REQUIRE_FALSE(Capture::Write(stream, Capture::Title, 0, 0, 0, 0, header, code, buffers));
    buffers[0] = {1, 0, Capture::MaxBufferBytes + 1, std::vector<u8>(Capture::MaxBufferBytes + 1)};
    REQUIRE_FALSE(Capture::Write(stream, Capture::Title, 0, 0, 0, 0, header, code, buffers));
    REQUIRE(stream.str().empty());
}

TEST_CASE("Native shader capture respects the maximum file budget and stream errors",
          "[video_core][native_capture]") {
    std::array<u8, 128> header{};
    std::vector<u8> code(Capture::MaxNativeBytes);
    std::array<Capture::Buffer, Capture::MaxBuffers> buffers{};
    for (auto& buffer : buffers)
        buffer = {1, 0, Capture::MaxBufferBytes, std::vector<u8>(Capture::MaxBufferBytes)};
    std::ostringstream stream(std::ios::binary);
    REQUIRE(Capture::Write(stream, Capture::Title, 0, 0, 0, 0, header, code, buffers));
    REQUIRE(stream.str().size() == 56 + 128 + Capture::MaxNativeBytes +
                                   Capture::MaxBuffers * (24 + Capture::MaxBufferBytes));
    REQUIRE(stream.str().size() * Capture::MaxRecords < 40 * 1024 * 1024);
    stream.setstate(std::ios::badbit);
    REQUIRE_FALSE(Capture::Write(stream, Capture::Title, 0, 0, 0, 0, header, code, buffers));
}
}
