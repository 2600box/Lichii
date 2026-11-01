// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <ostream>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::NativeShaderCapture {

inline constexpr u64 Title = 0x04007EF01F8AE000ULL;
inline constexpr size_t MaxRecords = 16;
inline constexpr size_t MaxNativeBytes = 1024 * 1024;
inline constexpr size_t MaxBuffers = 18;
inline constexpr size_t MaxBufferBytes = 65536;

struct Buffer {
    u32 status{};
    u64 address{};
    u32 declared_size{};
    std::vector<u8> bytes;
};

class Budget {
public:
    bool Reserve(u64 key) {
        if (count == keys.size()) return false;
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] == key) return false;
        }
        keys[count++] = key;
        return true;
    }

private:
    std::array<u64, MaxRecords> keys{};
    size_t count{};
};

inline bool Write(std::ostream& stream, u64 title, u64 address, u64 key, u32 bound_bank,
                  u32 error_offset, std::span<const u8> header, std::span<const u8> native,
                  std::span<const Buffer> buffers) {
    if (title != Title || header.size() != 128 || native.empty() ||
        native.size() > MaxNativeBytes || native.size() % 16 ||
        error_offset >= native.size() || error_offset % 16 || buffers.size() != MaxBuffers)
        return false;
    for (const auto& buffer : buffers) {
        if (buffer.status > 3 || buffer.bytes.size() > MaxBufferBytes ||
            (buffer.status == 1 && (buffer.bytes.size() != buffer.declared_size || buffer.bytes.empty())) ||
            (buffer.status != 1 && !buffer.bytes.empty())) return false;
    }
    const auto number = [&](u64 value, size_t size) {
        for (size_t i = 0; i < size; ++i) stream.put(static_cast<char>(value >> (8 * i)));
    };
    const auto bytes = [&](std::span<const u8> data) {
        if (!data.empty()) stream.write(reinterpret_cast<const char*>(data.data()), data.size());
    };
    stream.write("EGLACAP1", 8);
    number(title, 8);
    number(address, 8);
    number(key, 8);
    number(4, 4);
    number(header.size(), 4);
    number(native.size(), 4);
    number(buffers.size(), 4);
    number(bound_bank, 4);
    number(error_offset, 4);
    bytes(header);
    bytes(native);
    for (size_t i = 0; i < buffers.size(); ++i) {
        const auto& buffer = buffers[i];
        number(i, 4);
        number(buffer.status, 4);
        number(buffer.address, 8);
        number(buffer.declared_size, 4);
        number(buffer.bytes.size(), 4);
        bytes(buffer.bytes);
    }
    return static_cast<bool>(stream);
}

}
