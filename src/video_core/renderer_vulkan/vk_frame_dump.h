// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class ImageView;
class MemoryAllocator;
class Scheduler;

/// EDEN_NX2_FRAME_DUMP=<dir> reads back one whole frame: the color targets of every pass, the
/// images written by every compute dispatch and the presented image. EDEN_NX2_FRAME_DUMP_AT picks
/// the frame, in seconds after boot ("45") or as a frame number ("2700f").
/// EDEN_NX2_FRAME_DUMP_SAMPLED=1 also reads back every texture a pass samples (3D and depth
/// included), once per pass, as the first draw sampling it sees it.
class FrameDump {
public:
    struct Sampled {
        u32 stage;
        u32 tic;
        ImageView* view;
    };

    explicit FrameDump(const Device& device, MemoryAllocator& memory_allocator,
                       Scheduler& scheduler);
    ~FrameDump();

    [[nodiscard]] bool Capturing() const noexcept {
        return capturing;
    }

    [[nodiscard]] bool CapturingSampled() const noexcept {
        return capturing && sampled;
    }

    /// Reads back the textures of the current draw not yet seen in this pass. Returns true when
    /// copies were recorded, which ends the current render pass.
    bool NoteSampled(std::span<const Sampled> textures);

    /// Starts a pass when the framebuffer changes and captures the previous one.
    /// Returns true when copies were recorded, which ends the current render pass.
    bool BeginPass(const void* framebuffer, std::span<ImageView* const> colors);
    void NoteDraw(std::span<const u64> shader_hashes);
    /// Captures the current pass; returns true when copies were recorded.
    bool EndPass();
    void CaptureDispatch(u64 shader_hash, std::span<ImageView* const> written);
    void CapturePresent(ImageView& view);
    /// Waits for pending readbacks and writes them once they pass the staging budget.
    void DrainIfFull();
    void TickFrame();

private:
    enum class DepthEncoding { None, Unorm16, Unorm24, Float32 };

    struct Target {
        u32 slot;
        VkImage image;
        VideoCore::Surface::PixelFormat format;
        u32 width;
        u32 height;
        u32 depth;
        s32 level;
        s32 layer;
        GPUVAddr gpu_addr;
        bool rescaled;
        bool msaa;
        DepthEncoding depth_encoding;
    };

    struct Readback {
        vk::Buffer buffer;
        Target target;
        std::string name;
        std::string tags;
    };

    std::optional<Target> MakeTarget(ImageView& view, u32 slot) const;
    static u32 TexelBytes(const Target& target);
    static int Decode(const Target& target, const u8* pixel, std::array<float, 4>& out);
    bool Record(const Target& target, std::string name, std::string tags);
    void Drain();
    void Write(const Readback& readback, std::span<const u8> data);
    void WriteFile(const std::string& name, std::span<const u8> data);

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;

    bool enabled{};
    bool sampled{};
    bool capturing{};
    bool done{};
    bool truncated{};
    std::filesystem::path directory;
    std::filesystem::path trigger_file;
    u64 trigger_frame{~0ULL};
    double trigger_seconds{};
    std::chrono::steady_clock::time_point start;
    u64 frame{};

    std::ofstream manifest;
    std::vector<Readback> readbacks;
    size_t pending_bytes{};
    size_t written_bytes{};
    u32 sequence{};
    u32 images{};

    const void* pass_framebuffer{};
    std::vector<Target> pass_targets;
    std::vector<u64> pass_hashes;
    std::vector<std::pair<GPUVAddr, u32>> pass_sampled;
    u32 pass_index{};
    u32 pass_first_draw{};
    u32 draws{};
    u32 dispatches{};
};

} // namespace Vulkan
