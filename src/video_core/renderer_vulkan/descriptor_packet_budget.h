// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace Vulkan {

class DescriptorPacketBudget {
public:
    static constexpr size_t Limit = 1024 * 1024;

    template <typename Drain>
    bool Reserve(size_t bytes, Drain&& drain) {
        if (bytes > Limit) {
            return false;
        }
        if (bytes > Limit - pending) {
            drain();
            Reset();
        }
        pending += bytes;
        return true;
    }

    bool Pending() const noexcept {
        return pending != 0;
    }

    void Reset() noexcept {
        pending = 0;
    }

private:
    size_t pending{};
};

} // namespace Vulkan
