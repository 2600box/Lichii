// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "common/logging.h"
#include "common/thread_worker.h"
#include "video_core/renderer_vulkan/async_draw_policy.h"
#include "video_core/shader_jobs.h"

namespace Vulkan {

class EmissionBudget {
public:
    class Reservation {
    public:
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        ~Reservation() {
            if (pending) {
                pending->fetch_sub(1, std::memory_order_relaxed);
            }
        }

    private:
        friend class EmissionBudget;
        Reservation() = default;
        std::shared_ptr<std::atomic<size_t>> pending;
    };

    explicit EmissionBudget(size_t capacity_) : capacity{capacity_} {}

    std::unique_ptr<Reservation> TryAcquire(bool native) {
        size_t count = pending->load(std::memory_order_relaxed);
        if (!DefersShaderEmission(native, count, capacity)) {
            return nullptr;
        }
        auto reservation = std::unique_ptr<Reservation>{new Reservation};
        while (DefersShaderEmission(native, count, capacity)) {
            if (pending->compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
                reservation->pending = pending;
                return reservation;
            }
        }
        return nullptr;
    }

    size_t Pending() const {
        return pending->load(std::memory_order_relaxed);
    }

private:
    const size_t capacity;
    std::shared_ptr<std::atomic<size_t>> pending = std::make_shared<std::atomic<size_t>>(0);
};

/// Where a pipeline is built. Without workers or jobs it is built in its constructor.
struct PipelineBuildQueue {
    Common::ThreadWorker* workers{};
    /// Shader cache v2: a Soon job of this scheduler, whose CPU stage is the pipeline's deferred
    /// SPIR-V emission if it has one, and whose driver stage builds it. The estimates are the job's.
    VideoCommon::ShaderJobScheduler* jobs{};
    u64 cpu_bytes{};
    u64 driver_bytes{};
    /// Shader cache v2: left for Build(), the driver stage of the job that translated it
    bool owner_builds{};
};

class PipelineBuild {
    struct State {
        explicit State(std::function<void()> task_) : task{std::move(task_)} {}

        void Run() noexcept {
            if (claimed.exchange(true)) {
                return;
            }
            try {
                task();
            } catch (const std::exception& exception) {
                failed = true;
                LOG_ERROR(Render_Vulkan, "Graphics pipeline worker failed: {}", exception.what());
            } catch (...) {
                failed = true;
                LOG_ERROR(Render_Vulkan,
                          "Graphics pipeline worker failed with an unknown exception");
            }
            task = {};
            Complete();
        }

        /// Ends a build that never ran, as failed
        void Fail() noexcept {
            if (claimed.exchange(true)) {
                return;
            }
            failed = true;
            task = {};
            Complete();
        }

        void Complete() {
            std::scoped_lock lock{mutex};
            complete.store(true, std::memory_order_release);
            condition.notify_all();
        }

        std::function<void()> task;
        bool failed{};
        std::atomic_bool claimed{};
        std::atomic_bool complete{};
        std::mutex mutex;
        std::condition_variable condition;
    };

public:
    PipelineBuild(Common::ThreadWorker* workers, std::function<void()> task)
        : PipelineBuild(PipelineBuildQueue{.workers = workers}, std::move(task)) {}

    /// With jobs, `emit` is the CPU stage that runs before `task`
    PipelineBuild(const PipelineBuildQueue& queue, std::function<void()> task,
                  std::function<void()> emit = {})
        : state{std::make_shared<State>(std::move(task))}, jobs{queue.jobs},
          owner_builds{queue.owner_builds} {
        if (jobs) {
            VideoCommon::ShaderJob job{
                .priority = VideoCommon::ShaderJobPriority::Soon,
                .cpu_bytes = queue.cpu_bytes,
                .driver_bytes = queue.driver_bytes,
            };
            if (emit) {
                job.cpu = [state = state, emit = std::move(emit)](
                              VideoCommon::ShaderJobContext&) mutable -> VideoCommon::ShaderDriverStage {
                    try {
                        emit();
                    } catch (const std::exception& exception) {
                        LOG_ERROR(Render_Vulkan, "Graphics pipeline emission failed: {}",
                                  exception.what());
                        state->Fail();
                        return {};
                    } catch (...) {
                        LOG_ERROR(Render_Vulkan, "Graphics pipeline emission failed");
                        state->Fail();
                        return {};
                    }
                    return [state] { state->Run(); };
                };
            } else {
                job.driver = [state = state] { state->Run(); };
            }
            handle = jobs->Submit(std::move(job));
        } else if (queue.workers) {
            queue.workers->QueueWork([state = state] { state->Run(); });
        } else if (!owner_builds) {
            state->Run();
        }
    }

    ~PipelineBuild() {
        if (jobs) {
            // Nothing needs a pipeline that goes: a build still queued is dropped.
            jobs->Cancel(handle);
            handle.Wait();
        } else if (!owner_builds) {
            Wait(true);
        }
    }

    bool IsBuilt() const {
        return state->complete.load(std::memory_order_acquire);
    }

    bool Failed() const {
        return IsBuilt() && state->failed;
    }

    /// Builds it on this thread (owner_builds)
    void Build() const {
        state->Run();
    }

    void Wait(bool may_build = false) const {
        const auto shared = state;
        if (jobs) {
            if (IsBuilt()) {
                return;
            }
            // A draw waits for it. Its deferred emission is never run here.
            if (may_build) {
                jobs->RunOrWait(handle);
            } else {
                jobs->Promote(handle, VideoCommon::ShaderJobPriority::NeedNow);
                handle.Wait();
            }
            // Cancelled when the scheduler shut down
            shared->Fail();
            return;
        }
        if (may_build) {
            shared->Run();
        }
        std::unique_lock lock{shared->mutex};
        shared->condition.wait(lock,
                               [&] { return shared->complete.load(std::memory_order_acquire); });
    }

    /// Waits up to `timeout` for the build, without running it on this thread. The worker sets
    /// `complete` under the mutex before notifying, so the wakeup cannot be lost.
    /// @return True when the pipeline is built.
    bool WaitFor(std::chrono::microseconds timeout) const {
        const auto shared = state;
        if (jobs && !IsBuilt()) {
            jobs->Promote(handle, VideoCommon::ShaderJobPriority::NeedNow);
        }
        std::unique_lock lock{shared->mutex};
        return shared->condition.wait_for(lock, timeout, [&] {
            return shared->complete.load(std::memory_order_acquire);
        });
    }

    PipelineBuild(const PipelineBuild&) = delete;
    PipelineBuild& operator=(const PipelineBuild&) = delete;

private:
    std::shared_ptr<State> state;
    VideoCommon::ShaderJobScheduler* jobs{};
    VideoCommon::ShaderJobHandle handle;
    bool owner_builds{};
};

} // namespace Vulkan
