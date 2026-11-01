// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string_view>
#include <vulkan/vulkan_core.h>

namespace Vulkan {

constexpr VkInstanceCreateFlags InstanceFlagsForExtensions(std::span<const char* const> extensions) {
    for (const std::string_view extension : extensions) {
        if (extension == VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) {
            return VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    }
    // Direct MoltenVK and conformant ICDs need not advertise this loader extension.
    // Setting the flag without enabling the extension violates vkCreateInstance.
    return 0;
}

} // namespace Vulkan
