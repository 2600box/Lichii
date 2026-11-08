// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include "video_core/shader_jobs.h"

namespace {
using namespace std::chrono_literals;
using VideoCommon::ShaderDriverStage;
using VideoCommon::ShaderJob;
using VideoCommon::ShaderJobConfig;
using VideoCommon::ShaderJobContext;
using VideoCommon::ShaderJobHandle;
using VideoCommon::ShaderJobPriority;
using VideoCommon::ShaderJobScheduler;
using VideoCommon::ShaderJobStatus;
using enum VideoCommon::ShaderJobPriority;

class Gate {
public:
    void Open() {
        std::scoped_lock lock{mutex};
        open = true;
        condition.notify_all();
    }

    void Wait() {
        std::unique_lock lock{mutex};
        condition.wait(lock, [this] { return open; });
    }

private:
    std::mutex mutex;
    std::condition_variable condition;
    bool open{};
};

class Order {
public:
    void Add(int id) {
        std::scoped_lock lock{mutex};
        ids.push_back(id);
    }

    std::vector<int> Get() {
        std::scoped_lock lock{mutex};
        return ids;
    }

private:
    std::mutex mutex;
    std::vector<int> ids;
};

/// Sets a flag when destroyed, to see when a job's captures are released.
struct Tracker {
    explicit Tracker(std::atomic<bool>& destroyed_) : destroyed{destroyed_} {}
    ~Tracker() {
        destroyed = true;
    }
    std::atomic<bool>& destroyed;
};

ShaderJobConfig Config(size_t cpu_threads, size_t driver_threads, u64 budget = 1ULL << 40) {
    return {.cpu_threads = cpu_threads,
            .driver_threads = driver_threads,
            .memory_budget = budget,
            .background_aging = std::chrono::hours{1},
            .name = "TestJobs"};
}

template <typename F>
ShaderJob CpuJob(ShaderJobPriority priority, F&& body, u64 cpu_bytes = 0) {
    ShaderJob job;
    job.priority = priority;
    job.cpu_bytes = cpu_bytes;
    job.cpu = [body = std::forward<F>(body)](ShaderJobContext& context) mutable {
        if constexpr (std::is_invocable_v<F&, ShaderJobContext&>) {
            body(context);
        } else {
            body();
        }
        return ShaderDriverStage{};
    };
    return job;
}

template <typename Cpu, typename Driver>
ShaderJob TwoStageJob(ShaderJobPriority priority, Cpu&& cpu, Driver&& driver, u64 cpu_bytes = 0,
                      u64 driver_bytes = 0) {
    ShaderJob job;
    job.priority = priority;
    job.cpu_bytes = cpu_bytes;
    job.driver_bytes = driver_bytes;
    job.cpu = [cpu = std::forward<Cpu>(cpu),
               driver = std::forward<Driver>(driver)](ShaderJobContext& context) mutable {
        cpu(context);
        return ShaderDriverStage{std::move(driver)};
    };
    return job;
}

/// Submits a NeedNow job that holds a CPU worker until the gate opens.
ShaderJobHandle Block(ShaderJobScheduler& scheduler, Gate& gate) {
    auto entered = std::make_shared<std::promise<void>>();
    auto started = entered->get_future();
    auto handle = scheduler.Submit(CpuJob(NeedNow, [&gate, entered] {
        entered->set_value();
        gate.Wait();
    }));
    started.wait();
    return handle;
}

void WaitForStatus(const ShaderJobHandle& handle, ShaderJobStatus status) {
    while (handle.Status() != status) {
        std::this_thread::sleep_for(1ms);
    }
}

} // namespace

TEST_CASE("Shader jobs run the highest priority first", "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(1, 1)};
    Gate gate;
    const auto blocker = Block(scheduler, gate);
    Order order;
    std::vector<ShaderJobHandle> jobs;
    jobs.push_back(scheduler.Submit(CpuJob(Background, [&] { order.Add(1); })));
    jobs.push_back(scheduler.Submit(CpuJob(Soon, [&] { order.Add(2); })));
    jobs.push_back(scheduler.Submit(CpuJob(NeedNow, [&] { order.Add(3); })));
    jobs.push_back(scheduler.Submit(CpuJob(Soon, [&] { order.Add(4); })));
    jobs.push_back(scheduler.Submit(CpuJob(Background, [&] { order.Add(5); })));
    const auto queued = scheduler.Stats();
    gate.Open();
    for (const auto& job : jobs) {
        job.Wait();
    }
    blocker.Wait();
    const auto stats = scheduler.Stats();

    REQUIRE(order.Get() == std::vector<int>{3, 2, 4, 1, 5});
    REQUIRE(queued.priorities[0].running == 1);
    REQUIRE(queued.priorities[0].queued == 1);
    REQUIRE(queued.priorities[1].queued == 2);
    REQUIRE(queued.priorities[2].queued == 2);
    REQUIRE(stats.priorities[0].done == 2);
    REQUIRE(stats.priorities[1].done == 2);
    REQUIRE(stats.priorities[2].done == 2);
    for (const auto& priority : stats.priorities) {
        REQUIRE(priority.queued == 0);
        REQUIRE(priority.running == 0);
    }
    REQUIRE(stats.priorities[2].queue_wait_ns > 0);
    REQUIRE(stats.priorities[2].max_queue_wait_ns <= stats.priorities[2].queue_wait_ns);
}

TEST_CASE("Shader jobs promote queued jobs in both queues", "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(1, 1)};
    SECTION("CPU queue") {
        Gate gate;
        const auto blocker = Block(scheduler, gate);
        Order order;
        const auto first = scheduler.Submit(CpuJob(Background, [&] { order.Add(1); }));
        const auto second = scheduler.Submit(CpuJob(Background, [&] { order.Add(2); }));
        const auto third = scheduler.Submit(CpuJob(Background, [&] { order.Add(3); }));
        scheduler.Promote(third, NeedNow);
        scheduler.Promote(second, Soon);
        scheduler.Promote(third, Background);
        const auto promoted = scheduler.Stats();
        gate.Open();
        first.Wait();
        second.Wait();
        third.Wait();
        blocker.Wait();

        REQUIRE(third.Priority() == NeedNow);
        REQUIRE(second.Priority() == Soon);
        REQUIRE(first.Priority() == Background);
        REQUIRE(promoted.priorities[0].queued == 1);
        REQUIRE(promoted.priorities[1].queued == 1);
        REQUIRE(promoted.priorities[2].queued == 1);
        REQUIRE(scheduler.Stats().priorities[0].done == 2);
        REQUIRE(order.Get() == std::vector<int>{3, 2, 1});
    }
    SECTION("Driver queue") {
        Gate gate;
        std::promise<void> entered;
        const auto blocker = scheduler.Submit(TwoStageJob(
            NeedNow, [](ShaderJobContext&) {},
            [&] {
                entered.set_value();
                gate.Wait();
            }));
        entered.get_future().wait();
        Order order;
        std::vector<ShaderJobHandle> jobs;
        for (int id = 1; id <= 3; ++id) {
            jobs.push_back(scheduler.Submit(TwoStageJob(
                Background, [](ShaderJobContext&) {}, [&order, id] { order.Add(id); })));
        }
        for (const auto& job : jobs) {
            WaitForStatus(job, ShaderJobStatus::DriverQueued);
        }
        scheduler.Promote(jobs[2], NeedNow);
        gate.Open();
        for (const auto& job : jobs) {
            job.Wait();
        }
        blocker.Wait();
        REQUIRE(order.Get() == std::vector<int>{3, 1, 2});
    }
}

TEST_CASE("Shader jobs age Background jobs into Soon order", "[video_core][shader_jobs]") {
    for (const bool aging : {true, false}) {
        auto config = Config(1, 1);
        config.background_aging = aging ? std::chrono::nanoseconds{10ms} : std::chrono::hours{1};
        ShaderJobScheduler scheduler{config};
        Gate gate;
        const auto blocker = Block(scheduler, gate);
        Order order;
        const auto background = scheduler.Submit(CpuJob(Background, [&] { order.Add(1); }));
        std::this_thread::sleep_for(50ms);
        const auto soon = scheduler.Submit(CpuJob(Soon, [&] { order.Add(2); }));
        const auto soon2 = scheduler.Submit(CpuJob(Soon, [&] { order.Add(3); }));
        const auto need = scheduler.Submit(CpuJob(NeedNow, [&] { order.Add(4); }));
        gate.Open();
        background.Wait();
        soon.Wait();
        soon2.Wait();
        need.Wait();
        blocker.Wait();
        REQUIRE(order.Get() ==
                (aging ? std::vector<int>{4, 1, 2, 3} : std::vector<int>{4, 2, 3, 1}));
    }
}

TEST_CASE("Shader job budget admits by in-flight bytes", "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(4, 1, 100)};
    Gate first_gate, later_gate;
    std::promise<void> first_entered;
    const auto first = scheduler.Submit(CpuJob(
        Soon,
        [&] {
            first_entered.set_value();
            first_gate.Wait();
        },
        60));
    first_entered.get_future().wait();
    std::atomic<int> entered{};
    const auto second = scheduler.Submit(CpuJob(
        Soon,
        [&] {
            ++entered;
            later_gate.Wait();
        },
        60));
    const auto third = scheduler.Submit(CpuJob(
        Background,
        [&] {
            ++entered;
            later_gate.Wait();
        },
        30));
    std::this_thread::sleep_for(50ms);
    const auto held = scheduler.Stats();
    const bool both_queued =
        second.Status() == ShaderJobStatus::Queued && third.Status() == ShaderJobStatus::Queued;
    first_gate.Open();
    while (entered != 2) {
        std::this_thread::sleep_for(1ms);
    }
    const auto running = scheduler.Stats();
    later_gate.Open();
    first.Wait();
    second.Wait();
    third.Wait();
    const auto stats = scheduler.Stats();

    REQUIRE(both_queued);
    REQUIRE(held.in_flight_bytes == 60);
    REQUIRE(held.budget_stalls == 1);
    REQUIRE(running.in_flight_bytes == 90);
    REQUIRE(stats.in_flight_bytes == 0);
    REQUIRE(stats.peak_in_flight_bytes == 90);
    REQUIRE(stats.budget_stalls == 1);
    REQUIRE(stats.budget_stall_ns > 0);
    REQUIRE(stats.budget_bytes == 100);

    // A job larger than the budget still runs once nothing else is in flight.
    const auto large = scheduler.Submit(CpuJob(Background, [] {}, 1000));
    large.Wait();
    REQUIRE(large.Status() == ShaderJobStatus::Done);
}

TEST_CASE("Shader job CPU memory is released before the driver stage",
          "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(2, 1, 100)};
    std::atomic<bool> cpu_freed{};
    bool freed_at_driver{};
    Gate driver_gate;
    std::promise<void> driver_entered;
    const auto first = scheduler.Submit(TwoStageJob(
        Soon,
        [tracker = std::make_shared<Tracker>(cpu_freed)](ShaderJobContext& context) {
            context.ReportCpuBytes(70);
            context.SetDriverBytes(10);
        },
        [&] {
            freed_at_driver = cpu_freed;
            driver_entered.set_value();
            driver_gate.Wait();
        },
        80, 50));
    driver_entered.get_future().wait();
    const auto in_driver = scheduler.Stats();
    // 10 driver bytes plus 80 fit the budget of 100; the 80 CPU bytes no longer count.
    const auto second = scheduler.Submit(CpuJob(Soon, [] {}, 80));
    const bool second_ran = second.WaitFor(5s);
    driver_gate.Open();
    first.Wait();
    const auto stats = scheduler.Stats();

    REQUIRE(freed_at_driver);
    REQUIRE(in_driver.in_flight_bytes == 10);
    REQUIRE(second_ran);
    REQUIRE(stats.estimated_cpu_bytes == 80);
    REQUIRE(stats.reported_cpu_bytes == 70);
    REQUIRE(stats.peak_in_flight_bytes == 90);
    REQUIRE(stats.in_flight_bytes == 0);
}

TEST_CASE("NeedNow shader jobs bypass the budget", "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(3, 1, 100)};
    Gate gate;
    std::promise<void> soon_entered, need_entered;
    const auto soon = scheduler.Submit(CpuJob(
        Soon,
        [&] {
            soon_entered.set_value();
            gate.Wait();
        },
        90));
    soon_entered.get_future().wait();
    const auto held = scheduler.Submit(CpuJob(Soon, [] {}, 50));
    const auto need = scheduler.Submit(CpuJob(
        NeedNow,
        [&] {
            need_entered.set_value();
            gate.Wait();
        },
        50));
    need_entered.get_future().wait();
    const auto stats = scheduler.Stats();
    const bool held_queued = held.Status() == ShaderJobStatus::Queued;
    gate.Open();
    soon.Wait();
    held.Wait();
    need.Wait();

    REQUIRE(held_queued);
    REQUIRE(stats.in_flight_bytes == 140);
    REQUIRE(stats.peak_in_flight_bytes == 140);
    REQUIRE(held.Status() == ShaderJobStatus::Done);
}

TEST_CASE("Shader jobs cancel queued and running jobs", "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(1, 1)};
    SECTION("Queued") {
        Gate gate;
        const auto blocker = Block(scheduler, gate);
        std::atomic<bool> released{};
        bool ran{};
        const auto job = scheduler.Submit(CpuJob(
            Background, [&ran, tracker = std::make_shared<Tracker>(released)] { ran = true; }));
        scheduler.Cancel(job);
        const bool cancelled_at_once = job.Status() == ShaderJobStatus::Cancelled;
        const bool released_at_once = released;
        job.Wait();
        gate.Open();
        blocker.Wait();
        REQUIRE(cancelled_at_once);
        REQUIRE(released_at_once);
        REQUIRE_FALSE(ran);
        REQUIRE(scheduler.Stats().priorities[2].cancelled == 1);
    }
    SECTION("Running CPU stage") {
        bool driver_ran{};
        std::promise<void> entered;
        const auto job = scheduler.Submit(TwoStageJob(
            Soon,
            [&](ShaderJobContext& context) {
                entered.set_value();
                while (!context.StopRequested()) {
                    std::this_thread::sleep_for(1ms);
                }
            },
            [&] { driver_ran = true; }));
        entered.get_future().wait();
        scheduler.Cancel(job);
        job.Wait();
        REQUIRE(job.Status() == ShaderJobStatus::Cancelled);
        REQUIRE_FALSE(driver_ran);
    }
    SECTION("Running driver stage completes") {
        Gate gate;
        std::promise<void> entered;
        const auto job = scheduler.Submit(TwoStageJob(
            Soon, [](ShaderJobContext&) {},
            [&] {
                entered.set_value();
                gate.Wait();
            }));
        entered.get_future().wait();
        scheduler.Cancel(job);
        const bool still_running = job.Status() == ShaderJobStatus::Driver;
        gate.Open();
        job.Wait();
        REQUIRE(still_running);
        REQUIRE(job.Status() == ShaderJobStatus::Done);
    }
}

TEST_CASE("Cancelling a shader job batch leaves the workers running", "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(1, 1)};
    auto boot = scheduler.CreateBatch();
    std::promise<void> entered;
    const auto running = scheduler.Submit(CpuJob(Soon,
                                                 [&](ShaderJobContext& context) {
                                                     entered.set_value();
                                                     while (!context.StopRequested()) {
                                                         std::this_thread::sleep_for(1ms);
                                                     }
                                                 }),
                                          &boot);
    entered.get_future().wait();
    std::vector<ShaderJobHandle> queued;
    for (int i = 0; i < 3; ++i) {
        queued.push_back(
            scheduler.Submit(TwoStageJob(Soon, [](ShaderJobContext&) {}, [] {}), &boot));
    }
    bool other_ran{};
    const auto other = scheduler.Submit(CpuJob(Background, [&] { other_ran = true; }));
    const bool busy = !boot.Idle();
    scheduler.Cancel(boot);
    bool queued_cancelled = true;
    for (const auto& job : queued) {
        queued_cancelled = queued_cancelled && job.Status() == ShaderJobStatus::Cancelled;
    }
    boot.Wait();
    other.Wait();
    const auto late = scheduler.Submit(CpuJob(Soon, [] {}), &boot);
    const auto after = scheduler.Submit(CpuJob(Soon, [] {}));
    after.Wait();

    REQUIRE(busy);
    REQUIRE(queued_cancelled);
    REQUIRE(running.Status() == ShaderJobStatus::Cancelled);
    REQUIRE(boot.StopRequested());
    REQUIRE(boot.Idle());
    REQUIRE(other_ran);
    REQUIRE(other.Status() == ShaderJobStatus::Done);
    REQUIRE(late.Status() == ShaderJobStatus::Cancelled);
    REQUIRE(after.Status() == ShaderJobStatus::Done);
}

TEST_CASE("Shader job scheduler shuts down with queued and running jobs",
          "[video_core][shader_jobs]") {
    auto scheduler = std::make_unique<ShaderJobScheduler>(Config(2, 1));
    Gate gate;
    std::promise<void> cpu_entered, two_stage_entered;
    bool cpu_finished{}, two_stage_finished{}, driver_ran{};
    const auto cpu_only = scheduler->Submit(CpuJob(Soon, [&] {
        cpu_entered.set_value();
        gate.Wait();
        cpu_finished = true;
    }));
    const auto two_stage = scheduler->Submit(TwoStageJob(
        Soon,
        [&](ShaderJobContext&) {
            two_stage_entered.set_value();
            gate.Wait();
            two_stage_finished = true;
        },
        [&] { driver_ran = true; }));
    cpu_entered.get_future().wait();
    two_stage_entered.get_future().wait();
    std::atomic<bool> queued_released{};
    const auto queued = scheduler->Submit(
        CpuJob(Background, [tracker = std::make_shared<Tracker>(queued_released)] {}));
    auto queued_waiter = std::async(std::launch::async, [&] { queued.Wait(); });
    auto running_waiter = std::async(std::launch::async, [&] { two_stage.Wait(); });
    auto shutdown = std::async(std::launch::async, [&] { scheduler->Shutdown(); });
    const bool queued_woken = queued_waiter.wait_for(5s) == std::future_status::ready;
    const bool running_held = running_waiter.wait_for(20ms) == std::future_status::timeout;
    const bool shutdown_held = shutdown.wait_for(0ms) == std::future_status::timeout;
    gate.Open();
    shutdown.get();
    running_waiter.get();
    const auto late = scheduler->Submit(CpuJob(NeedNow, [] {}));
    scheduler.reset();

    REQUIRE(queued_woken);
    REQUIRE(queued.Status() == ShaderJobStatus::Cancelled);
    REQUIRE(queued_released);
    REQUIRE(running_held);
    REQUIRE(shutdown_held);
    REQUIRE(cpu_finished);
    REQUIRE(cpu_only.Status() == ShaderJobStatus::Done);
    REQUIRE(two_stage_finished);
    REQUIRE_FALSE(driver_ran);
    REQUIRE(two_stage.Status() == ShaderJobStatus::Cancelled);
    REQUIRE(late.Status() == ShaderJobStatus::Cancelled);
}

TEST_CASE("Shader job stage exceptions fail only that job", "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(1, 1)};
    const auto cpu_throws =
        scheduler.Submit(CpuJob(Soon, [] { throw std::runtime_error{"translation failed"}; }, 10));
    const auto driver_throws =
        scheduler.Submit(TwoStageJob(Soon, [](ShaderJobContext&) {}, [] { throw 7; }, 10, 20));
    cpu_throws.Wait();
    driver_throws.Wait();
    const auto after = scheduler.Submit(TwoStageJob(Soon, [](ShaderJobContext&) {}, [] {}));
    after.Wait();
    const auto stats = scheduler.Stats();

    REQUIRE(cpu_throws.Status() == ShaderJobStatus::Failed);
    REQUIRE(driver_throws.Status() == ShaderJobStatus::Failed);
    REQUIRE(after.Status() == ShaderJobStatus::Done);
    REQUIRE_FALSE(after.Exception());
    bool caught_message{};
    try {
        std::rethrow_exception(cpu_throws.Exception());
    } catch (const std::runtime_error& e) {
        caught_message = std::string{e.what()} == "translation failed";
    }
    bool caught_int{};
    try {
        std::rethrow_exception(driver_throws.Exception());
    } catch (int value) {
        caught_int = value == 7;
    }
    REQUIRE(caught_message);
    REQUIRE(caught_int);
    REQUIRE(stats.priorities[1].failed == 2);
    REQUIRE(stats.priorities[1].done == 1);
    REQUIRE(stats.in_flight_bytes == 0);
}

TEST_CASE("RunOrWait runs a queued job on the waiting thread", "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(1, 1)};
    Gate gate;
    const auto blocker = Block(scheduler, gate);
    std::thread::id cpu_thread, driver_thread;
    const auto job = scheduler.Submit(TwoStageJob(
        Background, [&](ShaderJobContext&) { cpu_thread = std::this_thread::get_id(); },
        [&] { driver_thread = std::this_thread::get_id(); }));
    const bool timed_out = !job.WaitFor(10ms);
    scheduler.RunOrWait(job);
    const auto stats = scheduler.Stats();
    gate.Open();
    scheduler.RunOrWait(blocker);

    REQUIRE(timed_out);
    REQUIRE(job.Status() == ShaderJobStatus::Done);
    REQUIRE(job.Priority() == NeedNow);
    REQUIRE(cpu_thread == std::this_thread::get_id());
    REQUIRE(driver_thread == std::this_thread::get_id());
    REQUIRE(stats.priorities[0].done == 1);
    REQUIRE(stats.priorities[0].running == 1);
    REQUIRE(blocker.Status() == ShaderJobStatus::Done);
}

#ifdef __linux__
TEST_CASE("Shader job workers are named by stage", "[video_core][shader_jobs]") {
    ShaderJobScheduler scheduler{Config(1, 1)};
    std::string cpu_name, driver_name;
    const auto job = scheduler.Submit(TwoStageJob(
        Soon,
        [&](ShaderJobContext&) {
            char name[16]{};
            pthread_getname_np(pthread_self(), name, sizeof(name));
            cpu_name = name;
        },
        [&] {
            char name[16]{};
            pthread_getname_np(pthread_self(), name, sizeof(name));
            driver_name = name;
        }));
    job.Wait();
    REQUIRE(cpu_name == "TestJobsCpu");
    REQUIRE(driver_name == "TestJobsDriver");
}
#endif

TEST_CASE("Shader jobs stay consistent with many producers", "[video_core][shader_jobs]") {
    auto config = Config(4, 2, 1000);
    config.background_aging = 1ms;
    ShaderJobScheduler scheduler{config};
    constexpr int Producers = 8;
    constexpr int JobsPerProducer = 250;
    struct Runs {
        std::atomic<int> cpu{};
        std::atomic<int> driver{};
    };
    std::vector<Runs> runs(Producers * JobsPerProducer);
    std::vector<ShaderJobHandle> handles(runs.size());
    auto batch = scheduler.CreateBatch();
    std::vector<std::thread> producers;
    for (int producer = 0; producer < Producers; ++producer) {
        producers.emplace_back([&, producer] {
            std::mt19937 random{static_cast<u32>(producer)};
            for (int i = 0; i < JobsPerProducer; ++i) {
                const size_t index = static_cast<size_t>(producer * JobsPerProducer + i);
                const auto priority = static_cast<ShaderJobPriority>(random() % 3);
                Runs& run = runs[index];
                handles[index] = scheduler.Submit(
                    TwoStageJob(
                        priority,
                        [&run](ShaderJobContext& context) {
                            ++run.cpu;
                            context.ReportCpuBytes(context.StopRequested() ? 0 : 100);
                        },
                        [&run] { ++run.driver; }, random() % 400, random() % 400),
                    i % 2 == 0 ? &batch : nullptr);
                const ShaderJobHandle& handle = handles[index];
                switch (random() % 10) {
                case 0:
                    scheduler.Cancel(handle);
                    break;
                case 1:
                    scheduler.Promote(handle, NeedNow);
                    break;
                case 2:
                    scheduler.RunOrWait(handle);
                    break;
                default:
                    break;
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    batch.Wait();
    for (const auto& handle : handles) {
        handle.Wait();
    }
    const auto stats = scheduler.Stats();
    u64 done{}, cancelled{}, failed{};
    for (const auto& priority : stats.priorities) {
        REQUIRE(priority.queued == 0);
        REQUIRE(priority.running == 0);
        done += priority.done;
        cancelled += priority.cancelled;
        failed += priority.failed;
    }
    u64 done_handles{};
    for (size_t index = 0; index < runs.size(); ++index) {
        const ShaderJobStatus status = handles[index].Status();
        REQUIRE(runs[index].cpu <= 1);
        REQUIRE(runs[index].driver <= 1);
        if (status == ShaderJobStatus::Done) {
            ++done_handles;
            REQUIRE(runs[index].cpu == 1);
            REQUIRE(runs[index].driver == 1);
        } else {
            REQUIRE(status == ShaderJobStatus::Cancelled);
            REQUIRE(runs[index].driver == 0);
        }
    }
    REQUIRE(done + cancelled == runs.size());
    REQUIRE(failed == 0);
    REQUIRE(done == done_handles);
    REQUIRE(cancelled > 0);
    REQUIRE(stats.in_flight_bytes == 0);
    REQUIRE(batch.Idle());
}
