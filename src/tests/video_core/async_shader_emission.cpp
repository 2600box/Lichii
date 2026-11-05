// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/thread_worker.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"

namespace {
struct GraphicsPrograms {
    Shader::ObjectPool<Shader::IR::Inst> instructions;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    std::array<Shader::IR::Program, 2> programs;

    GraphicsPrograms() {
        for (size_t stage = 0; stage < programs.size(); ++stage) {
            auto& program = programs[stage];
            auto* block = blocks.Create(instructions);
            program.stage = stage == 0 ? Shader::Stage::VertexB : Shader::Stage::Fragment;
            program.blocks = {block};
            program.post_order_blocks = {block};
            program.syntax_list = {
                {{.block = block}, Shader::IR::AbstractSyntaxNode::Type::Block},
                {{}, Shader::IR::AbstractSyntaxNode::Type::Return},
            };
            program.info.constant_buffer_descriptors = {{0, 1}};
            program.info.storage_buffers_descriptors = {{0, 0x110, 1, true}};
            program.info.used_storage_buffer_types = Shader::IR::Type::U32;
            Shader::IR::IREmitter ir{*block};
            block->AppendNewInst(Shader::IR::Opcode::WriteStorage32,
                                 {ir.Imm32(0), ir.Imm32(0), ir.Imm32(0x12345678)});
        }
    }

    std::array<std::vector<u32>, 2> Emit() {
        Shader::Profile profile{};
        profile.supported_spirv = 0x00010300;
        profile.unified_descriptor_binding = true;
        Shader::Backend::Bindings bindings;
        std::array<std::vector<u32>, 2> result;
        for (size_t stage = 0; stage < programs.size(); ++stage) {
            result[stage] =
                Shader::Backend::SPIRV::EmitSPIRV(profile, {}, programs[stage], bindings);
        }
        return result;
    }
};
} // namespace

TEST_CASE("Graphics IR survives transfer to the shader worker and emits identical SPIR-V",
          "[video_core][async_draw][spirv]") {
    const auto producer_thread = std::this_thread::get_id();
    std::promise<void> release;
    auto released = release.get_future().share();
    Common::ThreadWorker workers{1, "TestPipelineBuilder"};
    auto job = std::make_shared<GraphicsPrograms>();
    const auto reference = job->Emit();
    std::weak_ptr<GraphicsPrograms> lifetime = job;
    std::promise<std::array<std::vector<u32>, 2>> result;
    auto emitted = result.get_future();
    std::thread::id build_thread;
    workers.QueueWork([job = std::move(job), released, &result, &build_thread] {
        released.wait();
        build_thread = std::this_thread::get_id();
        try {
            result.set_value(job->Emit());
        } catch (...) {
            result.set_exception(std::current_exception());
        }
    });
    const bool held_by_worker = !lifetime.expired();
    const bool still_pending = emitted.wait_for(std::chrono::seconds{0}) != std::future_status::ready;
    release.set_value();
    const auto actual = emitted.get();
    workers.WaitForRequests();
    REQUIRE(held_by_worker);
    REQUIRE(still_pending);
    REQUIRE(build_thread != producer_thread);
    REQUIRE(actual == reference);
    REQUIRE(lifetime.expired());
}
