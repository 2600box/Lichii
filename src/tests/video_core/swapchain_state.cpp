// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/vk_swapchain_state.h"
#include "video_core/vulkan_common/vulkan_instance_flags.h"

TEST_CASE("Vulkan portability flags follow enabled extensions, not the host OS",
          "[video_core][vulkan][instance]") {
    REQUIRE(Vulkan::InstanceFlagsForExtensions({}) == 0);
    const std::array direct{VK_KHR_SURFACE_EXTENSION_NAME, "VK_EXT_metal_surface"};
    REQUIRE(Vulkan::InstanceFlagsForExtensions(direct) == 0);
    const std::array loader{VK_KHR_SURFACE_EXTENSION_NAME,
                            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    REQUIRE(Vulkan::InstanceFlagsForExtensions(loader) ==
            VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR);
}

TEST_CASE("Swapchain acquisition distinguishes valid images from resize and timeout results",
          "[video_core][vulkan][swapchain]") {
    using Vulkan::ClassifySwapchainAcquire;
    using Result = Vulkan::SwapchainAcquireResult;
    REQUIRE(ClassifySwapchainAcquire(VK_SUCCESS) == Result::Acquired);
    REQUIRE(ClassifySwapchainAcquire(VK_SUBOPTIMAL_KHR) == Result::Suboptimal);
    REQUIRE(ClassifySwapchainAcquire(VK_ERROR_OUT_OF_DATE_KHR) == Result::Outdated);
    REQUIRE(ClassifySwapchainAcquire(VK_TIMEOUT) == Result::Unavailable);
    REQUIRE(ClassifySwapchainAcquire(VK_NOT_READY) == Result::Unavailable);
    REQUIRE(ClassifySwapchainAcquire(VK_ERROR_SURFACE_LOST_KHR) == Result::Error);
    REQUIRE(ClassifySwapchainAcquire(VK_ERROR_DEVICE_LOST) == Result::Error);
    REQUIRE(ClassifySwapchainAcquire(VK_ERROR_OUT_OF_HOST_MEMORY) == Result::Error);
}

TEST_CASE("Swapchain extents handle fullscreen transitions and minimized surfaces",
          "[video_core][vulkan][swapchain]") {
    VkSurfaceCapabilitiesKHR capabilities{};
    capabilities.minImageExtent = {1, 1};
    capabilities.maxImageExtent = {4096, 4096};
    capabilities.currentExtent = {0, 0};
    REQUIRE_FALSE(Vulkan::IsSwapchainExtentDrawable(
        Vulkan::ChooseSwapExtent(capabilities, 1920, 1080)));
    capabilities.currentExtent = {1920, 0};
    REQUIRE_FALSE(Vulkan::IsSwapchainExtentDrawable(
        Vulkan::ChooseSwapExtent(capabilities, 1920, 1080)));
    capabilities.currentExtent = {2560, 1600};
    auto extent = Vulkan::ChooseSwapExtent(capabilities, 1920, 1080);
    REQUIRE(extent.width == 2560);
    REQUIRE(extent.height == 1600);
    REQUIRE(Vulkan::IsSwapchainExtentDrawable(extent));
    capabilities.currentExtent = {(std::numeric_limits<uint32_t>::max)(),
                                  (std::numeric_limits<uint32_t>::max)()};
    extent = Vulkan::ChooseSwapExtent(capabilities, 9000, 0);
    REQUIRE(extent.width == 4096);
    REQUIRE(extent.height == 1);
    capabilities.minImageExtent = {0, 0};
    capabilities.maxImageExtent = {0, 0};
    REQUIRE_FALSE(Vulkan::IsSwapchainExtentDrawable(
        Vulkan::ChooseSwapExtent(capabilities, 1920, 1080)));
}
