// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#ifdef __ANDROID__
#include <android/api-level.h>
#endif

#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "vulkan/vulkan_core.h"

namespace Vulkan {

namespace {

VkSurfaceFormatKHR ChooseSwapSurfaceFormat(vk::Span<VkSurfaceFormatKHR> formats) {
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        VkSurfaceFormatKHR format;
        format.format = VK_FORMAT_B8G8R8A8_UNORM;
        format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return format;
    }
    const auto& found = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    return found != formats.end() ? *found : formats[0];
}

static VkPresentModeKHR ChooseSwapPresentMode(bool has_imm, bool has_mailbox,
                                              bool has_fifo_relaxed) {
    // Mailbox doesn't lock the application like FIFO (vsync)
    // FIFO present mode locks the framerate to the monitor's refresh rate
    Settings::VSyncMode setting = [has_imm, has_mailbox]() {
        // Choose Mailbox or Immediate if unlocked and those modes are supported
        const auto mode = Settings::values.vsync_mode.GetValue();
        if (Settings::values.frame_gen.GetValue()) {
            return mode == Settings::VSyncMode::FifoRelaxed ? mode : Settings::VSyncMode::Fifo;
        }
        if (Settings::values.use_speed_limit.GetValue() &&
            Settings::values.current_speed_mode.GetValue() != Settings::SpeedMode::Turbo) {
            return mode;
        }
        switch (mode) {
        case Settings::VSyncMode::Fifo:
        case Settings::VSyncMode::FifoRelaxed:
            if (has_mailbox) {
                return Settings::VSyncMode::Mailbox;
            } else if (has_imm) {
                return Settings::VSyncMode::Immediate;
            }
            [[fallthrough]];
        default:
            return mode;
        }
    }();
    if (setting == Settings::VSyncMode::Immediate && !has_imm) {
        setting = Settings::VSyncMode::Mailbox;
    }
    if ((setting == Settings::VSyncMode::Mailbox && !has_mailbox) ||
        (setting == Settings::VSyncMode::FifoRelaxed && !has_fifo_relaxed)) {
        setting = Settings::VSyncMode::Fifo;
    }

    switch (setting) {
    case Settings::VSyncMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case Settings::VSyncMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case Settings::VSyncMode::Fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    case Settings::VSyncMode::FifoRelaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

VkCompositeAlphaFlagBitsKHR ChooseAlphaFlags(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown composite alpha flags value {:#x}",
                  capabilities.supportedCompositeAlpha);
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
}

} // Anonymous namespace

Swapchain::Swapchain(
    VkSurfaceKHR_T* surface_,
    const Device& device_,
    Scheduler& scheduler_,
    u32 width_,
    u32 height_)
    : surface(surface_)
    , device{device_}
    , scheduler{scheduler_}
{
    if (!Create(surface, width_, height_)) {
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
}

Swapchain::~Swapchain() = default;

bool Swapchain::Create(
    VkSurfaceKHR_T* surface_,
    u32 width_,
    u32 height_)
{
    const auto physical_device = device.GetPhysical();
    const auto capabilities{physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface_))};
    if (!IsSwapchainExtentDrawable(capabilities.maxImageExtent) ||
        !IsSwapchainExtentDrawable(ChooseSwapExtent(capabilities, width_, height_))) {
        is_outdated = true;
        return false;
    }

    // Presentation submits work outside the scheduler's timeline. Waiting for a
    // scheduler tick does not make its images or binary semaphores safe to free.
    // Serialize with both queue submit and present while draining that work.
    std::scoped_lock lock{scheduler.submit_mutex};
    if (swapchain) {
        vk::Check(device.GetLogical().WaitIdle());
    }
    Destroy();

    width = width_;
    height = height_;
    surface = surface_;
    is_outdated = true;
    if (!CreateSwapchain(capabilities)) {
        return false;
    }
    CreateSemaphores();

    is_outdated = false;
    is_suboptimal = false;
    return true;
}

SwapchainAcquireResult Swapchain::AcquireNextImage(VkSemaphore image_acquired) {
    // A finite wait lets minimized/transitioning windows release their queued frames.
    constexpr u64 acquire_timeout_ns = 100'000'000;
    const VkResult result = device.GetLogical().AcquireNextImageKHR(
        *swapchain, acquire_timeout_ns, image_acquired,
        VK_NULL_HANDLE, &image_index);
    const auto acquired = ClassifySwapchainAcquire(result);
    switch (acquired) {
    case SwapchainAcquireResult::Acquired:
        break;
    case SwapchainAcquireResult::Suboptimal:
        is_suboptimal = true;
        break;
    case SwapchainAcquireResult::Outdated:
        is_outdated = true;
        return acquired;
    case SwapchainAcquireResult::Unavailable:
        return acquired;
    case SwapchainAcquireResult::Error:
        LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));
        vk::Check(result);
        return acquired;
    }

    // Image acquisition, render_ready, and the frame's present_done fence order
    // the copy. An unrelated scheduler.CurrentTick() can still be unsubmitted;
    // waiting for it here stalls async presentation and does not protect WSI.
    const auto wait_with_frame_pacing = [this] {
        switch (Settings::values.frame_pacing_mode.GetValue()) {
        case Settings::FramePacingMode::Target_Auto:
            break;
        case Settings::FramePacingMode::Target_30:
            scheduler.Wait(0, 30.0, false);
            break;
        case Settings::FramePacingMode::Target_60:
            scheduler.Wait(0, 60.0, false);
            break;
        case Settings::FramePacingMode::Target_90:
            scheduler.Wait(0, 90.0, false);
            break;
        case Settings::FramePacingMode::Target_120:
            scheduler.Wait(0, 120.0, false);
            break;
        }
    };

#ifdef __ANDROID__
    // Android 11+ uses platform frame pacing.
    if (android_get_device_api_level() < 30) {
        wait_with_frame_pacing();
    }
#else
    wait_with_frame_pacing();
#endif

    return acquired;
}

void Swapchain::Present(VkSemaphore render_semaphore) {
    const auto present_queue{device.GetPresentQueue()};
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = render_semaphore ? 1U : 0U,
        .pWaitSemaphores = &render_semaphore,
        .swapchainCount = 1,
        .pSwapchains = swapchain.address(),
        .pImageIndices = &image_index,
        .pResults = nullptr,
    };
    std::scoped_lock lock{scheduler.submit_mutex};
    switch (const VkResult result = present_queue.Present(present_info)) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        LOG_DEBUG(Render_Vulkan, "Suboptimal swapchain");
        is_suboptimal = true;
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));
        vk::Check(result);
        break;
    }
    ++frame_index;
    if (frame_index >= image_count) {
        frame_index = 0;
    }
}

bool Swapchain::CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities) {
    const auto physical_device{device.GetPhysical()};
    const auto formats{physical_device.GetSurfaceFormatsKHR(VkSurfaceKHR(surface))};
    const auto present_modes = physical_device.GetSurfacePresentModesKHR(VkSurfaceKHR(surface));

    has_mailbox = std::find(present_modes.begin(), present_modes.end(), VK_PRESENT_MODE_MAILBOX_KHR)
                  != present_modes.end();
    has_imm = std::find(present_modes.begin(), present_modes.end(),
                        VK_PRESENT_MODE_IMMEDIATE_KHR) != present_modes.end();
    has_fifo_relaxed = std::find(present_modes.begin(), present_modes.end(),
                                 VK_PRESENT_MODE_FIFO_RELAXED_KHR) != present_modes.end();

    const VkCompositeAlphaFlagBitsKHR alpha_flags{ChooseAlphaFlags(capabilities)};
    surface_format = ChooseSwapSurfaceFormat(formats);
    present_mode = ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed);

    u32 requested_image_count{capabilities.minImageCount + 1};
    // Ensure Triple buffering if possible.
    if (capabilities.maxImageCount > 0) {
        if (requested_image_count > capabilities.maxImageCount) {
            requested_image_count = capabilities.maxImageCount;
        } else {
            requested_image_count =
                (std::max)(requested_image_count, (std::min)(3U, capabilities.maxImageCount));
        }
    } else {
        requested_image_count = (std::max)(requested_image_count, 3U);
    }
    VkSwapchainCreateInfoKHR swapchain_ci{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = VkSurfaceKHR(surface),
        .minImageCount = requested_image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = {},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
#ifdef __ANDROID__
        // On Android, do not allow surface rotation to deviate from the frontend.
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
#else
        .preTransform = capabilities.currentTransform,
#endif
        .compositeAlpha = alpha_flags,
        .presentMode = present_mode,
        .clipped = VK_FALSE,
        .oldSwapchain = VkSwapchainKHR{},
    };
    const u32 graphics_family{device.GetGraphicsFamily()};
    const u32 present_family{device.GetPresentFamily()};
    const std::array<u32, 2> queue_indices{graphics_family, present_family};
    if (graphics_family != present_family) {
        swapchain_ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_ci.queueFamilyIndexCount = static_cast<u32>(queue_indices.size());
        swapchain_ci.pQueueFamilyIndices = queue_indices.data();
    }
    // According to Vulkan spec, when using VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR,
    // the base format (imageFormat) MUST be included in pViewFormats
    const std::array view_formats{
        swapchain_ci.imageFormat,  // Base format MUST be first
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
#ifdef __ANDROID__
        VK_FORMAT_R8G8B8A8_UNORM,  // Android may use RGBA
        VK_FORMAT_R8G8B8A8_SRGB,
#endif
    };
    VkImageFormatListCreateInfo format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (device.IsKhrSwapchainMutableFormatEnabled()) {
        format_list.pNext = std::exchange(swapchain_ci.pNext, &format_list);
        swapchain_ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    }
    // Resizing can continue while we wait for the old swapchain to finish. Probe
    // again immediately before creation, including the zero-extent check.
    const auto updated = physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface));
    if (!IsSwapchainExtentDrawable(updated.maxImageExtent)) {
        return false;
    }
    swapchain_ci.imageExtent = ChooseSwapExtent(updated, width, height);
    if (!IsSwapchainExtentDrawable(swapchain_ci.imageExtent)) {
        return false;
    }
    swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);

    extent = swapchain_ci.imageExtent;

    images = swapchain.GetImages();
    image_count = static_cast<u32>(images.size());
#ifdef __ANDROID__
    // Android is already ordered the same as Switch.
    image_view_format = VK_FORMAT_R8G8B8A8_UNORM;
#else
    image_view_format = VK_FORMAT_B8G8R8A8_UNORM;
#endif
    return true;
}

void Swapchain::CreateSemaphores() {
    render_semaphores.resize(image_count);
    std::ranges::generate(render_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
}

void Swapchain::Destroy() {
    frame_index = 0;
    render_semaphores.clear();
    swapchain.reset();
}

void Swapchain::Release() {
    std::scoped_lock lock{scheduler.submit_mutex};
    if (swapchain) {
        vk::Check(device.GetLogical().WaitIdle());
        Destroy();
    }
    is_outdated = true;
}

bool Swapchain::NeedsPresentModeUpdate() const {
    const auto requested_mode = ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed);
    return present_mode != requested_mode;
}

} // namespace Vulkan
