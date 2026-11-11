// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <type_traits>

#include "common/bit_field.h"
#include "common/common_types.h"

#include "video_core/engines/maxwell_3d.h"
#include "video_core/surface.h"
#include "video_core/transform_feedback.h"

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct DynamicFeatures {
    u32 driver_id;
    u32 driver_version;
    bool has_extended_dynamic_state;
    bool has_extended_dynamic_state_2;
    bool has_extended_dynamic_state_2_logic_op;
    bool has_extended_dynamic_state_2_patch_control_points;
    bool has_extended_dynamic_state_3_blend;
    bool has_extended_dynamic_state_3_enables;
    bool has_dynamic_state3_depth_clamp_enable;
    bool has_dynamic_state3_logic_op_enable;
    bool has_dynamic_state3_line_stipple_enable;
    bool has_dynamic_vertex_input;
    bool has_static_vertex_repack;
    bool has_color_write_enable;
    bool has_provoking_vertex;
    bool has_provoking_vertex_first_mode;
    bool has_provoking_vertex_last_mode;
    bool has_provoking_vertex_tf_preserve;
};

struct FixedPipelineState {
    static u32 PackComparisonOp(Maxwell::ComparisonOp op) noexcept;
    static Maxwell::ComparisonOp UnpackComparisonOp(u32 packed) noexcept;

    static u32 PackStencilOp(Maxwell::StencilOp::Op op) noexcept;
    static Maxwell::StencilOp::Op UnpackStencilOp(u32 packed) noexcept;

    static u32 PackCullFace(Maxwell::CullFace cull) noexcept;
    static Maxwell::CullFace UnpackCullFace(u32 packed) noexcept;

    static u32 PackFrontFace(Maxwell::FrontFace face) noexcept;
    static Maxwell::FrontFace UnpackFrontFace(u32 packed) noexcept;

    static u32 PackPolygonMode(Maxwell::PolygonMode mode) noexcept;
    static Maxwell::PolygonMode UnpackPolygonMode(u32 packed) noexcept;

    static u32 PackLogicOp(Maxwell::LogicOp::Op op) noexcept;
    static Maxwell::LogicOp::Op UnpackLogicOp(u32 packed) noexcept;

    static u32 PackBlendEquation(Maxwell::Blend::Equation equation) noexcept;
    static Maxwell::Blend::Equation UnpackBlendEquation(u32 packed) noexcept;

    static u32 PackBlendFactor(Maxwell::Blend::Factor factor) noexcept;
    static Maxwell::Blend::Factor UnpackBlendFactor(u32 packed) noexcept;

    struct BlendingAttachment {
        union {
            u32 raw;
            BitField<0, 1, u32> mask_r;
            BitField<1, 1, u32> mask_g;
            BitField<2, 1, u32> mask_b;
            BitField<3, 1, u32> mask_a;
            BitField<4, 3, u32> equation_rgb;
            BitField<7, 3, u32> equation_a;
            BitField<10, 5, u32> factor_source_rgb;
            BitField<15, 5, u32> factor_dest_rgb;
            BitField<20, 5, u32> factor_source_a;
            BitField<25, 5, u32> factor_dest_a;
            BitField<30, 1, u32> enable;
        };

        void Refresh(const Maxwell& regs, size_t index);

        std::array<bool, 4> Mask() const noexcept {
            return {mask_r != 0, mask_g != 0, mask_b != 0, mask_a != 0};
        }

        Maxwell::Blend::Equation EquationRGB() const noexcept {
            return UnpackBlendEquation(equation_rgb.Value());
        }

        Maxwell::Blend::Equation EquationAlpha() const noexcept {
            return UnpackBlendEquation(equation_a.Value());
        }

        Maxwell::Blend::Factor SourceRGBFactor() const noexcept {
            return UnpackBlendFactor(factor_source_rgb.Value());
        }

        Maxwell::Blend::Factor DestRGBFactor() const noexcept {
            return UnpackBlendFactor(factor_dest_rgb.Value());
        }

        Maxwell::Blend::Factor SourceAlphaFactor() const noexcept {
            return UnpackBlendFactor(factor_source_a.Value());
        }

        Maxwell::Blend::Factor DestAlphaFactor() const noexcept {
            return UnpackBlendFactor(factor_dest_a.Value());
        }
    };

    union VertexAttribute {
        u32 raw;
        BitField<0, 1, u32> enabled;
        BitField<1, 5, u32> buffer;
        BitField<6, 14, u32> offset;
        BitField<20, 3, u32> type;
        BitField<23, 6, u32> size;

        Maxwell::VertexAttribute::Type Type() const noexcept {
            return static_cast<Maxwell::VertexAttribute::Type>(type.Value());
        }

        Maxwell::VertexAttribute::Size Size() const noexcept {
            return static_cast<Maxwell::VertexAttribute::Size>(size.Value());
        }
    };

    template <size_t Position>
    union StencilFace {
        BitField<Position + 0, 3, u32> action_stencil_fail;
        BitField<Position + 3, 3, u32> action_depth_fail;
        BitField<Position + 6, 3, u32> action_depth_pass;
        BitField<Position + 9, 3, u32> test_func;

        Maxwell::StencilOp::Op ActionStencilFail() const noexcept {
            return UnpackStencilOp(action_stencil_fail);
        }

        Maxwell::StencilOp::Op ActionDepthFail() const noexcept {
            return UnpackStencilOp(action_depth_fail);
        }

        Maxwell::StencilOp::Op ActionDepthPass() const noexcept {
            return UnpackStencilOp(action_depth_pass);
        }

        Maxwell::ComparisonOp TestFunc() const noexcept {
            return UnpackComparisonOp(test_func);
        }
    };

    struct DynamicState {
        union {
            u32 raw1;
            BitField<0, 2, u32> cull_face;
            BitField<2, 1, u32> cull_enable;
            BitField<3, 1, u32> primitive_restart_enable;
            BitField<4, 1, u32> depth_bias_enable;
            BitField<5, 1, u32> rasterize_enable;
            BitField<6, 4, u32> logic_op;
            BitField<10, 1, u32> logic_op_enable;
            BitField<11, 1, u32> depth_clamp_disabled;
            BitField<12, 1, u32> line_stipple_enable;
        };
        union {
            u32 raw2;
            StencilFace<0> front;
            StencilFace<12> back;
            BitField<24, 1, u32> stencil_enable;
            BitField<25, 1, u32> depth_write_enable;
            BitField<26, 1, u32> depth_bounds_enable;
            BitField<27, 1, u32> depth_test_enable;
            BitField<28, 1, u32> front_face;
            BitField<29, 3, u32> depth_test_func;
        };

        void Refresh(const Maxwell& regs);
        void Refresh2(const Maxwell& regs, Maxwell::PrimitiveTopology topology,
                      bool base_features_supported);
        void Refresh3(const Maxwell& regs, const DynamicFeatures& features);

        Maxwell::ComparisonOp DepthTestFunc() const noexcept {
            return UnpackComparisonOp(depth_test_func);
        }

        Maxwell::CullFace CullFace() const noexcept {
            return UnpackCullFace(cull_face.Value());
        }

        Maxwell::FrontFace FrontFace() const noexcept {
            return UnpackFrontFace(front_face.Value());
        }
    };

    union {
        u32 raw1;
        BitField<0, 1, u32> extended_dynamic_state;
        BitField<1, 1, u32> extended_dynamic_state_2;
        BitField<2, 1, u32> extended_dynamic_state_2_logic_op;
        BitField<3, 1, u32> extended_dynamic_state_3_blend;
        BitField<4, 1, u32> extended_dynamic_state_3_enables;
        BitField<5, 1, u32> dynamic_vertex_input;
        BitField<6, 1, u32> xfb_enabled;
        BitField<7, 1, u32> ndc_minus_one_to_one;
        BitField<8, 2, u32> polygon_mode;
        BitField<10, 2, u32> tessellation_primitive;
        BitField<12, 2, u32> tessellation_spacing;
        BitField<14, 1, u32> tessellation_clockwise;
        BitField<15, 5, u32> patch_control_points_minus_one;
        BitField<20, 1, u32> color_write_enable_dynamic;

        BitField<21, 1, u32> attachment0_dual_source_blend;

        BitField<24, 4, Maxwell::PrimitiveTopology> topology;
        BitField<28, 4, Tegra::Texture::MsaaMode> msaa_mode;
    };
    union {
        u32 raw2;
        BitField<1, 3, u32> alpha_test_func;
        BitField<4, 1, u32> early_z;
        BitField<5, 1, u32> depth_enabled;
        BitField<6, 5, u32> depth_format;
        BitField<11, 1, u32> y_negate;
        BitField<12, 1, u32> provoking_vertex_last;
        BitField<13, 1, u32> conservative_raster_enable;
        BitField<14, 1, u32> smooth_lines;
        BitField<15, 1, u32> alpha_to_coverage_enabled;
        BitField<16, 1, u32> alpha_to_one_enabled;
        BitField<17, 3, Tegra::Engines::Maxwell3D::EngineHint> app_stage;
    };
    std::array<u8, Maxwell::NumRenderTargets> color_formats;

    u32 driver_id;
    u32 driver_version;
    u32 alpha_test_ref;
    u32 point_size;

    std::array<u16, Maxwell::NumViewports> viewport_swizzles;
    union {
        u64 attribute_types; // Used with VK_EXT_vertex_input_dynamic_state
        u64 enabled_divisors;
    };

    DynamicState dynamic_state;
    std::array<BlendingAttachment, Maxwell::NumRenderTargets> attachments;
    std::array<VertexAttribute, Maxwell::NumVertexAttributes> attributes;
    std::array<u32, Maxwell::NumVertexArrays> binding_divisors;
    // Vertex stride is a 12 bits value, we have 4 bits to spare per element
    std::array<u16, Maxwell::NumVertexArrays> vertex_strides;

    VideoCommon::TransformFeedbackState xfb_state;

    u32 depth_bounds_min;
    u32 depth_bounds_max;

    u32 line_stipple_factor;
    u32 line_stipple_pattern;

    void Refresh(Tegra::Engines::Maxwell3D& maxwell3d, DynamicFeatures& features);

    size_t Hash() const noexcept;

    bool operator==(const FixedPipelineState& rhs) const noexcept;

    bool operator!=(const FixedPipelineState& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        if (xfb_enabled) {
            // When transform feedback is enabled, use the whole struct
            return sizeof(*this);
        }
        if (dynamic_vertex_input && extended_dynamic_state_3_blend) {
            return offsetof(FixedPipelineState, attachments);
        }
        if (dynamic_vertex_input) {
            // Exclude dynamic state
            return offsetof(FixedPipelineState, attributes);
        }
        if (extended_dynamic_state) {
            // Exclude dynamic state
            return offsetof(FixedPipelineState, vertex_strides);
        }
        // Default
        return offsetof(FixedPipelineState, xfb_state);
    }

    u32 DynamicAttributeType(size_t index) const noexcept {
        return (attribute_types >> (index * 2)) & 0b11;
    }
};
static_assert(std::has_unique_object_representations_v<FixedPipelineState>);
static_assert(std::is_trivially_copyable_v<FixedPipelineState>);
static_assert(std::is_trivially_constructible_v<FixedPipelineState>);

/// Dynamic states a pipeline key can be made for, one bit each. A key gets exactly the ones the
/// device has (FixedPipelineState::Refresh), so a key naming one the device lacks never matches.
namespace KeyDynamicState {
constexpr u32 VertexInput = 1U << 0;
constexpr u32 Extended = 1U << 1;
constexpr u32 Extended2 = 1U << 2;
constexpr u32 Extended2LogicOp = 1U << 3;
constexpr u32 Extended3Blend = 1U << 4;
constexpr u32 Extended3Enables = 1U << 5;
constexpr u32 ColorWriteEnable = 1U << 6;
} // namespace KeyDynamicState

/// The KeyDynamicState bits `state` was made with.
[[nodiscard]] inline u32 KeyDynamicStates(const FixedPipelineState& state) noexcept {
    return (state.dynamic_vertex_input != 0 ? KeyDynamicState::VertexInput : 0U) |
           (state.extended_dynamic_state != 0 ? KeyDynamicState::Extended : 0U) |
           (state.extended_dynamic_state_2 != 0 ? KeyDynamicState::Extended2 : 0U) |
           (state.extended_dynamic_state_2_logic_op != 0 ? KeyDynamicState::Extended2LogicOp
                                                         : 0U) |
           (state.extended_dynamic_state_3_blend != 0 ? KeyDynamicState::Extended3Blend : 0U) |
           (state.extended_dynamic_state_3_enables != 0 ? KeyDynamicState::Extended3Enables
                                                        : 0U) |
           (state.color_write_enable_dynamic != 0 ? KeyDynamicState::ColorWriteEnable : 0U);
}

/// The KeyDynamicState bits the device lacks.
[[nodiscard]] inline u32 MissingKeyDynamicStates(const DynamicFeatures& features) noexcept {
    return (features.has_dynamic_vertex_input ? 0U : KeyDynamicState::VertexInput) |
           (features.has_extended_dynamic_state ? 0U : KeyDynamicState::Extended) |
           (features.has_extended_dynamic_state_2 ? 0U : KeyDynamicState::Extended2) |
           (features.has_extended_dynamic_state_2_logic_op ? 0U
                                                           : KeyDynamicState::Extended2LogicOp) |
           (features.has_extended_dynamic_state_3_blend ? 0U : KeyDynamicState::Extended3Blend) |
           (features.has_extended_dynamic_state_3_enables ? 0U
                                                          : KeyDynamicState::Extended3Enables) |
           (features.has_color_write_enable ? 0U : KeyDynamicState::ColorWriteEnable);
}

/// KeyDynamicState bits whose static form is color blend state only: the logic op (EDS2) and the
/// color write masks (color write enable). Neither reaches the shader libraries nor the SPIR-V,
/// and the key holds a valid static value for each (logic op 0 = CLEAR, the guest write masks),
/// so a key made with one, built without it, gives the libraries a device lacking it builds at
/// runtime. The other states leave their static fields unfilled in the key (cull mode, depth
/// test, rasterizer discard, vertex attributes, blend equations, depth clamp), which the
/// libraries would then bake in: those entries stay left out.
constexpr u32 AdaptableKeyDynamicStates =
    KeyDynamicState::Extended2LogicOp | KeyDynamicState::ColorWriteEnable;

/// Clears from `state` the adaptable dynamic states (AdaptableKeyDynamicStates) in `missing`, the
/// states the device lacks. Returns the bits it cleared.
inline u32 AdaptKeyDynamicStates(FixedPipelineState& state, u32 missing) noexcept {
    const u32 cleared = KeyDynamicStates(state) & missing & AdaptableKeyDynamicStates;
    if ((cleared & KeyDynamicState::Extended2LogicOp) != 0) {
        state.extended_dynamic_state_2_logic_op.Assign(0);
    }
    if ((cleared & KeyDynamicState::ColorWriteEnable) != 0) {
        state.color_write_enable_dynamic.Assign(0);
    }
    return cleared;
}

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::FixedPipelineState> {
    size_t operator()(const Vulkan::FixedPipelineState& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std
