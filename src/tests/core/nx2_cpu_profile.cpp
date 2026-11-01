// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fiber.h"
#include "core/nx2_cpu_profile.h"

using namespace Core::Nx2CpuProfile;

TEST_CASE("Nx2CpuProfile: per-core split and SVC order", "[core][nx2_cpu_profile]") {
    Window window;
    for (int i = 0; i < 50; ++i) {
        window.AddSample(0, static_cast<u32>(State::Guest));
    }
    for (int i = 0; i < 30; ++i) {
        window.AddSample(0, static_cast<u32>(State::Idle));
    }
    for (int i = 0; i < 10; ++i) {
        window.AddSample(0, static_cast<u32>(State::Readback));
    }
    for (int i = 0; i < 8; ++i) {
        window.AddSample(0, SvcBase + 0x21);
    }
    for (int i = 0; i < 2; ++i) {
        window.AddSample(0, SvcBase + 0x18);
    }
    window.AddSvcCalls(0x21, 200);
    window.AddSvcCalls(0x18, 1000);
    window.AddSvcCalls(0x1d, 3);

    const auto lines = window.Report(10.0);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0] ==
            "core 0: guest 50.0% | svc 10.0% | readback 10.0% | emulator 0.0% | idle 30.0%");
    REQUIRE(lines[1] == "svcs by sampled time | 0x21: 8 samples, 20 calls/s"
                        " | 0x18: 2 samples, 100 calls/s | 0x1d: 0 samples, 0 calls/s");
}

TEST_CASE("Nx2CpuProfile: cores without samples are left out", "[core][nx2_cpu_profile]") {
    Window window;
    window.AddSample(3, static_cast<u32>(State::Emulator));
    const auto lines = window.Report(1.0);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] ==
            "core 3: guest 0.0% | svc 0.0% | readback 0.0% | emulator 100.0% | idle 0.0%");
}

namespace {
u32 Value(State state) {
    return static_cast<u32>(state);
}

u32 Read(size_t core) {
    return GetShared().states[core].load(std::memory_order_relaxed);
}

u64 TakeCalls(u32 svc) {
    return GetShared().svc_calls[svc].exchange(0);
}
} // namespace

TEST_CASE("Nx2CpuProfile: scopes nest and restore the core's state", "[core][nx2_cpu_profile]") {
    std::vector<u32> seen;
    std::thread{[&] {
        const CoreRegistration registration{2, true};
        {
            const Scope guest{State::Guest};
            seen.push_back(Read(2));
            {
                const Scope readback{State::Readback};
                seen.push_back(Read(2));
            }
            seen.push_back(Read(2));
        }
        seen.push_back(Read(2));
        {
            const SvcScope svc{0x34};
            seen.push_back(Read(2));
        }
        seen.push_back(Read(2));
    }}.join();
    REQUIRE(seen == std::vector<u32>{Value(State::Guest), Value(State::Readback),
                                     Value(State::Guest), Value(State::Emulator), SvcBase + 0x34,
                                     Value(State::Emulator)});
    REQUIRE(TakeCalls(0x34) == 1);
}

TEST_CASE("Nx2CpuProfile: threads that are not cores change nothing", "[core][nx2_cpu_profile]") {
    TakeCalls(0x35);
    std::vector<u32> seen;
    std::thread{[&] {
        const CoreRegistration registration{3, false};
        const Scope guest{State::Guest};
        const SvcScope svc{0x35};
        const SwitchScope fiber_switch;
        seen.push_back(Read(3));
    }}.join();
    REQUIRE(seen == std::vector<u32>{Value(State::Emulator)});
    REQUIRE(!GetShared().registered[3].load());
    REQUIRE(TakeCalls(0x35) == 0);
}

TEST_CASE("Nx2CpuProfile: a parked thread keeps its state across a fiber switch and migration",
          "[core][nx2_cpu_profile]") {
    using Common::Fiber;
    const CoreRegistration session{5, true};
    TakeCalls(0x34);
    std::vector<u32> seen;
    std::shared_ptr<Fiber> host0;
    std::shared_ptr<Fiber> host1;
    std::shared_ptr<Fiber> a;
    std::shared_ptr<Fiber> b;

    // Thread A blocks in WaitForAddress on core 0 and resumes on core 1.
    a = std::make_shared<Fiber>([&] {
        {
            const SvcScope svc{0x34};
            seen.push_back(Read(0));
            {
                const SwitchScope park;
                Fiber::YieldTo(a, *host0);
            }
            seen.push_back(Read(1));
        }
        seen.push_back(Read(1));
        Fiber::YieldTo(a, *host1);
    });
    // Thread B runs guest code on core 0 while A is parked.
    b = std::make_shared<Fiber>([&] {
        seen.push_back(Read(0));
        {
            const Scope guest{State::Guest};
            seen.push_back(Read(0));
        }
        seen.push_back(Read(0));
        Fiber::YieldTo(b, *host0);
    });

    std::thread{[&] {
        const CoreRegistration registration{0, true};
        host0 = Fiber::ThreadToFiber();
        Fiber::YieldTo(host0, *a);
        seen.push_back(Read(0));
        Fiber::YieldTo(host0, *b);
        seen.push_back(Read(0));
        host0->Exit();
    }}.join();
    std::thread{[&] {
        const CoreRegistration registration{1, true};
        host1 = Fiber::ThreadToFiber();
        Fiber::YieldTo(host1, *a);
        seen.push_back(Read(1));
        host1->Exit();
    }}.join();

    const u32 svc = SvcBase + 0x34;
    const u32 emulator = Value(State::Emulator);
    REQUIRE(seen == std::vector<u32>{svc, emulator, emulator, Value(State::Guest), emulator,
                                     emulator, svc, emulator, emulator});
    REQUIRE(TakeCalls(0x34) == 1);
}

TEST_CASE("Nx2CpuProfile: the sampler runs while cores are registered", "[core][nx2_cpu_profile]") {
    auto& shared = GetShared();
    REQUIRE(!shared.sampler.joinable());
    {
        const CoreRegistration core4{4, true};
        REQUIRE(shared.sampler.joinable());
        bool registered5 = false;
        std::thread{[&] {
            const CoreRegistration core5{5, true};
            registered5 = shared.registered[5].load();
        }}.join();
        REQUIRE(registered5);
        REQUIRE(!shared.registered[5].load());
        REQUIRE(shared.sampler.joinable());
        const Scope guest{State::Guest};
        REQUIRE(Read(4) == Value(State::Guest));
    }
    REQUIRE(!shared.sampler.joinable());
    REQUIRE(!shared.registered[4].load());
    REQUIRE(Read(4) == Value(State::Emulator));
    REQUIRE(CurrentCore() == NumCores);
}

TEST_CASE("Nx2CpuProfile: an SVC spanning window boundaries is counted once",
          "[core][nx2_cpu_profile]") {
    TakeCalls(0x34);
    std::vector<std::string> reports;
    const auto close_window = [&reports] {
        Window window;
        DrainSvcCalls(window, GetShared());
        const auto lines = window.Report(1.0);
        reports.push_back(lines.empty() ? std::string{} : lines.back());
    };
    std::thread{[&] {
        const CoreRegistration registration{2, true};
        {
            const SvcScope svc{0x34};
            close_window();
            close_window();
        }
        close_window();
        const SvcScope svc{0x34};
        const SvcScope nested{0x35};
        close_window();
    }}.join();
    REQUIRE(reports == std::vector<std::string>{
                           "svcs by sampled time | 0x34: 0 samples, 1 calls/s",
                           "",
                           "",
                           "svcs by sampled time | 0x34: 0 samples, 1 calls/s"
                           " | 0x35: 0 samples, 1 calls/s",
                       });
}
