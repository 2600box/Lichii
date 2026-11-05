// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "video_core/renderer_vulkan/vk_update_descriptor.h"

namespace Vulkan {

struct DescriptorBufferBinding {
    VkDescriptorType type;
    u32 count;
    VkDeviceSize offset;
    VkDeviceSize stride;
};

struct DescriptorBufferLayout {
    VkDeviceSize size{};
    boost::container::small_vector<DescriptorBufferBinding, 32> bindings;

    [[nodiscard]] bool Empty() const noexcept {
        return bindings.empty();
    }
};

class DescriptorBufferPacket {
public:
    enum class Comparison { NotRequested, Equal, DifferentOpaqueBytes };

    static size_t Bytes(const DescriptorBufferLayout& layout, size_t entries, bool verify) {
        return sizeof(DescriptorBufferPacket) + entries * sizeof(DescriptorUpdateEntry) +
               layout.bindings.size() * sizeof(DescriptorBufferBinding) +
               (verify ? 2 * static_cast<size_t>(layout.size) : 0);
    }

    template <typename Encode>
    DescriptorBufferPacket(const DescriptorBufferLayout& layout_,
                           std::span<const DescriptorUpdateEntry> entries_, u8* destination_,
                           bool verify, Encode&& encode)
        : layout{layout_}, entries{entries_.begin(), entries_.end()}, destination{destination_} {
        if (verify) {
            comparison = std::make_unique<u8[]>(2 * layout.size);
            encode(layout_, entries_.data(), comparison.get());
        }
    }

    template <typename Encode>
    Comparison Execute(Encode&& encode) const {
        if (!comparison) {
            encode(layout, entries.data(), destination);
            return Comparison::NotRequested;
        }
        u8* const expected = comparison.get();
        u8* const actual = expected + layout.size;
        encode(layout, entries.data(), actual);
        bool equal = true;
        for (const auto& binding : layout.bindings) {
            equal &= std::memcmp(expected + binding.offset, actual + binding.offset,
                                 binding.count * binding.stride) == 0;
        }
        const u8* const selected = equal ? actual : expected;
        for (const auto& binding : layout.bindings) {
            std::memcpy(destination + binding.offset, selected + binding.offset,
                        binding.count * binding.stride);
        }
        return equal ? Comparison::Equal : Comparison::DifferentOpaqueBytes;
    }

private:
    DescriptorBufferLayout layout;
    std::vector<DescriptorUpdateEntry> entries;
    u8* destination;
    std::unique_ptr<u8[]> comparison;
};

template <typename Scheduler, typename Encode, typename ReportDifference>
void RecordDescriptorBufferPacket(Scheduler& scheduler, const DescriptorBufferLayout& layout,
                                  std::span<const DescriptorUpdateEntry> entries, u8* host,
                                  bool enabled, bool verify, Encode encode,
                                  ReportDifference report_difference) {
    if (!enabled || !scheduler.ReserveDescriptorBytes(
                        DescriptorBufferPacket::Bytes(layout, entries.size(), verify))) {
        encode(layout, entries.data(), host);
        return;
    }
    DescriptorBufferPacket packet{layout, entries, host, verify, encode};
    scheduler.Record([packet = std::move(packet), encode = std::move(encode),
                      report_difference = std::move(report_difference)](vk::CommandBuffer) {
        if (packet.Execute(encode) == DescriptorBufferPacket::Comparison::DifferentOpaqueBytes) {
            report_difference();
        }
    });
}

} // namespace Vulkan
