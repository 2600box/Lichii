// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone model of the HostTiming thread's wait (src/core/core_timing.cpp). It compares
// waiting the whole time in a hardware wait (what Event::WaitFor does on x86 Windows) with
// sleeping in the OS until a margin before the deadline and hardware-waiting the rest.
// See tools/nx2/hosttiming-wait-bench.md.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define BENCH_X64 1
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::nanoseconds;

long long NowNs() {
    return std::chrono::duration_cast<nanoseconds>(Clock::now().time_since_epoch()).count();
}

long long ThreadCpuNs() {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
    const auto ticks = [](FILETIME t) {
        return (static_cast<long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
#else
    timespec time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return static_cast<long long>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
#endif
}

bool HasMonitorx() {
#ifdef BENCH_X64
#ifdef _MSC_VER
    int regs[4];
    __cpuid(regs, 0x80000001);
    return (regs[2] >> 29) & 1;
#else
    unsigned eax, ebx, ecx, edx;
    return __get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx) && ((ecx >> 29) & 1);
#endif
#else
    return false;
#endif
}

const bool g_monitorx = HasMonitorx() && std::getenv("BENCH_NO_MWAITX") == nullptr;
double g_tsc_per_ns = 0.0;

#ifdef BENCH_X64
#ifdef __clang__
__attribute__((target("mwaitx")))
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("mwaitx")
#endif
void MwaitxFor(const void* address, long long ns) {
    _mm_monitorx(const_cast<void*>(address), 0, 0);
    const double cycles = static_cast<double>(ns) * g_tsc_per_ns;
    _mm_mwaitx(1 << 1, 0, static_cast<unsigned>(std::min(cycles + 1.0, 4294967295.0)));
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
#endif

void CalibrateTsc() {
#ifdef BENCH_X64
    const long long ns0 = NowNs();
    const unsigned long long tsc0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    g_tsc_per_ns = static_cast<double>(__rdtsc() - tsc0) / static_cast<double>(NowNs() - ns0);
#endif
}

// Common::Event reduced to what the timer thread uses.
class Wake {
public:
    Wake() {
#ifdef _WIN32
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        high_resolution = timer != nullptr;
        if (!timer) {
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
#endif
    }

    void Set() {
        std::scoped_lock lk{mutex};
        if (!is_set) {
            is_set = true;
#ifdef _WIN32
            if (sleeping) {
                SetEvent(wake);
            }
#endif
            condvar.notify_one();
        }
    }

    // Event::WaitFor on x86 Windows: MWAITX (or a pause loop) until signalled or the deadline.
    bool HardwareWait(long long deadline) {
        for (;;) {
            bool expected = true;
            if (is_set.compare_exchange_strong(expected, false)) {
                return true;
            }
            const long long remaining = deadline - NowNs();
            if (remaining <= 0) {
                return false;
            }
#ifdef BENCH_X64
            if (g_monitorx) {
                MwaitxFor(&is_set, remaining);
            } else {
                _mm_pause();
            }
#else
            std::this_thread::yield();
#endif
        }
    }

    // Event::SleepFor: blocks in the OS until signalled or the deadline.
    bool Sleep(long long deadline) {
        std::unique_lock lk{mutex};
#ifdef _WIN32
        while (!is_set) {
            const long long remaining = deadline - NowNs();
            if (remaining <= 0) {
                return false;
            }
            LARGE_INTEGER due;
            due.QuadPart = -((remaining + 99) / 100);
            SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
            sleeping = true;
            lk.unlock();
            const HANDLE handles[]{wake, timer};
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            lk.lock();
            sleeping = false;
        }
#else
        const Clock::time_point until{nanoseconds{deadline}};
        if (!condvar.wait_until(lk, until, [this] { return is_set.load(); })) {
            return false;
        }
#endif
        is_set = false;
        return true;
    }

    bool high_resolution = false;

private:
    alignas(64) std::atomic<bool> is_set{false};
    std::mutex mutex;
    std::condition_variable condvar;
#ifdef _WIN32
    HANDLE wake{};
    HANDLE timer{};
    bool sleeping = false;
#endif
};

struct Samples {
    std::vector<long long> values;
    long long Percentile(double p) {
        if (values.empty()) {
            return 0;
        }
        const size_t index = std::min(values.size() - 1, static_cast<size_t>(p * values.size()));
        std::nth_element(values.begin(), values.begin() + index, values.end());
        return values[index];
    }
};

struct Result {
    long long waits = 0;
    long long woken = 0;
    long long late_sleeps = 0;
    long long sleep_ns = 0;
    long long hardware_ns = 0;
    long long cpu_ns = 0;
    long long elapsed_ns = 0;
    Samples wake_late;
    Samples event_late;
};

struct Options {
    double seconds = 5.0;
    int guest_rate = 500;
    std::vector<long long> margins_us;
};

// One run of the HostTiming loop. margin_us < 0 is the hardware wait only (the current code).
Result Run(const Options& options, long long margin_us) {
    std::mutex queue_mutex;
    std::priority_queue<long long, std::vector<long long>, std::greater<>> queue;
    Wake wake;
    std::atomic<bool> stop{false};
    const long long periods[]{5'000'000, 10'000'000, 15'000'000, 16'666'667};
    const long long start = NowNs();
    {
        std::scoped_lock lk{queue_mutex};
        for (const long long period : periods) {
            queue.push(start + period);
        }
    }
    const auto schedule = [&](long long time) {
        {
            std::scoped_lock lk{queue_mutex};
            queue.push(time);
        }
        wake.Set();
    };

    // Guest timers (KHardwareTimer) scheduled from another thread, 20 us to 3 ms ahead.
    std::thread guest([&] {
        std::mt19937_64 random{12345};
        std::uniform_real_distribution<double> log_delay{std::log(20e3), std::log(3e6)};
        std::exponential_distribution<double> gap{static_cast<double>(options.guest_rate) / 1e9};
        while (!stop.load()) {
            std::this_thread::sleep_for(nanoseconds{static_cast<long long>(gap(random))});
            schedule(NowNs() + static_cast<long long>(std::exp(log_delay(random))));
        }
    });

    Result result;
    const long long margin = margin_us * 1000;
    const long long end = start + static_cast<long long>(options.seconds * 1e9);
    const long long cpu_start = ThreadCpuNs();
    std::vector<long long> periodic(std::begin(periods), std::end(periods));
    for (;;) {
        long long next;
        {
            std::unique_lock lk{queue_mutex};
            for (long long now = NowNs(); !queue.empty() && queue.top() <= now; now = NowNs()) {
                const long long due = queue.top();
                queue.pop();
                result.event_late.values.push_back(now - due);
                for (const long long period : periodic) {
                    if ((due - start) % period == 0) {
                        queue.push(due + period);
                        break;
                    }
                }
            }
            next = queue.top();
        }
        long long now = NowNs();
        if (now >= end) {
            break;
        }
        if (next <= now) {
            continue;
        }
        bool signalled = false;
        if (margin >= 0 && next - now > margin) {
            signalled = wake.Sleep(next - margin);
            const long long woke = NowNs();
            result.sleep_ns += woke - now;
            result.late_sleeps += !signalled && woke > next;
            now = woke;
        }
        if (!signalled && next > now) {
            signalled = wake.HardwareWait(next);
            const long long woke = NowNs();
            result.hardware_ns += woke - now;
            now = woke;
        }
        ++result.waits;
        if (signalled) {
            ++result.woken;
        } else {
            result.wake_late.values.push_back(now - next);
        }
    }
    result.cpu_ns = ThreadCpuNs() - cpu_start;
    result.elapsed_ns = NowNs() - start;
    stop = true;
    guest.join();
    return result;
}

void SetHighPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    using SetResolution = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
    using QueryResolution = LONG(NTAPI*)(PULONG, PULONG, PULONG);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto set = reinterpret_cast<SetResolution>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetTimerResolution")));
    const auto query = reinterpret_cast<QueryResolution>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryTimerResolution")));
    if (set && query && std::getenv("BENCH_DEFAULT_RESOLUTION") == nullptr) {
        ULONG minimum, maximum, current;
        query(&minimum, &maximum, &current);
        set(maximum, TRUE, &current);
        std::printf("timer resolution %.3f ms (the emulator requests the maximum too)\n",
                    current / 10000.0);
    }
#else
    if (setpriority(PRIO_PROCESS, 0, -8) != 0) {
        std::printf("nice -8 not permitted; running at the default priority\n");
    }
#endif
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc) {
            options.seconds = std::atof(argv[++i]);
        } else if (arg == "--guest-rate" && i + 1 < argc) {
            options.guest_rate = std::atoi(argv[++i]);
        } else if (arg == "--margins" && i + 1 < argc) {
            std::string list = argv[++i];
            for (size_t pos = 0; pos <= list.size();) {
                const size_t comma = std::min(list.find(',', pos), list.size());
                options.margins_us.push_back(std::atoll(list.substr(pos, comma - pos).c_str()));
                pos = comma + 1;
            }
        } else {
            std::fprintf(stderr, "Usage: %s [--seconds S] [--guest-rate N] [--margins -1,0,500]\n"
                                 "  margin -1: hardware wait only (current Event::WaitFor)\n"
                                 "  margin M>=0: OS sleep until M us before the deadline\n",
                         argv[0]);
            return 2;
        }
    }
    if (options.margins_us.empty()) {
        options.margins_us = {-1, 0, 100, 250, 500, 1000, 2000};
    }
    SetHighPriority();
    CalibrateTsc();
    Wake probe;
    std::printf("hardware wait: %s; OS sleep: %s; %.1f s per run, %d guest timers/s plus 5, 10, "
                "15 and 16.7 ms periodic events\n",
                g_monitorx ? "MWAITX" : "pause loop",
#ifdef _WIN32
                probe.high_resolution ? "high-resolution waitable timer" : "waitable timer",
#else
                "condition_variable::wait_until (futex)",
#endif
                options.seconds, options.guest_rate);
    std::printf("%-8s %7s %6s %9s %9s %6s | %-27s | %-27s | %s\n", "margin", "waits", "woken",
                "sleep_ms", "hwait_ms", "cpu%", "wake late p50/p99/p999/max", 
                "event late p50/p99/p999/max", "sleeps past deadline");
    for (const long long margin : options.margins_us) {
        Result r = Run(options, margin);
        const double scale = 1000.0 / static_cast<double>(r.elapsed_ns) * 1e6;
        const auto us = [](long long ns) { return ns / 1000.0; };
        char wake[64], event[64];
        std::snprintf(wake, sizeof(wake), "%.1f/%.1f/%.1f/%.1f us",
                      us(r.wake_late.Percentile(0.5)), us(r.wake_late.Percentile(0.99)),
                      us(r.wake_late.Percentile(0.999)), us(r.wake_late.Percentile(1.0)));
        std::snprintf(event, sizeof(event), "%.1f/%.1f/%.1f/%.1f us",
                      us(r.event_late.Percentile(0.5)), us(r.event_late.Percentile(0.99)),
                      us(r.event_late.Percentile(0.999)), us(r.event_late.Percentile(1.0)));
        std::printf("%-8s %7lld %6lld %9.1f %9.1f %6.1f | %-27s | %-27s | %lld\n",
                    margin < 0 ? "hw-only" : std::to_string(margin).c_str(), r.waits, r.woken,
                    r.sleep_ns * scale / 1e6, r.hardware_ns * scale / 1e6,
                    100.0 * r.cpu_ns / r.elapsed_ns, wake, event, r.late_sleeps);
        std::fflush(stdout);
    }
    return 0;
}
