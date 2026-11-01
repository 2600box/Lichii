// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <limits>
#include <vulkan/vulkan_core.h>

namespace Vulkan {

enum class SwapchainAcquireResult { Acquired, Suboptimal, Outdated, Unavailable, Error };

constexpr SwapchainAcquireResult ClassifySwapchainAcquire(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return SwapchainAcquireResult::Acquired;
    case VK_SUBOPTIMAL_KHR:
        // The image index is valid and the acquire semaphore WILL be signaled.
        return SwapchainAcquireResult::Suboptimal;
    case VK_ERROR_OUT_OF_DATE_KHR:
        return SwapchainAcquireResult::Outdated;
    case VK_TIMEOUT:
    case VK_NOT_READY:
        return SwapchainAcquireResult::Unavailable;
    default:
        return SwapchainAcquireResult::Error;
    }
}

constexpr VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                    uint32_t width, uint32_t height) {
    if (capabilities.currentExtent.width != (std::numeric_limits<uint32_t>::max)()) {
        return capabilities.currentExtent;
    }
    return {
        std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}

constexpr bool IsSwapchainExtentDrawable(VkExtent2D extent) {
    return extent.width != 0 && extent.height != 0;
}

} // namespace Vulkan
