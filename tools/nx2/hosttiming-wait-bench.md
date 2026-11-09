# HostTiming wait

The HostTiming thread (`CoreTiming::Initialize` in `src/core/core_timing.cpp`) waits for the next
CoreTiming event with `Common::Event::WaitFor`. On x86 Windows that wait is MWAITX (AMD) or
UMWAIT (Intel) for the whole timeout (Eden #3984, 7c32cf03a1). It never enters the kernel, so
Windows counts the thread as running: 99.7-99.8% of a core in every game on the 5900X. Linux
already waits on a futex there.

This branch adds a test switch that sleeps in the OS for most of the wait:

| Variable | Effect |
|---|---|
| `EDEN_NX2_HOSTTIMING_WAIT=<us>` | Sleep (`Event::SleepFor`, a high-resolution waitable timer on Windows) until `<us>` microseconds before the next event, then `Event::WaitFor` for the rest. `0` sleeps the whole wait. Unset keeps the current wait. |
| `EDEN_NX2_HOSTTIMING_STATS=1` | One `Core.Timing` line per second: waits, waits woken by newly scheduled events, OS sleep and WaitFor time, sleeps that ended after the deadline, wake lateness and event lateness p50/p99/max, and the thread's CPU %. |

Both change live through the debug server: `host_timing {margin_us?, stats?}` (`margin_us` -1 is
the current wait). It returns the previous and current values. Read the stats with `log_tail`
(`contains: "HostTiming wait"`).

Wake lateness is how late a timed wait ended, which is what the wait controls. Event lateness is
how late each event's callback ran (its `ns_late`), which is what the guest sees. It also counts
events that were scheduled in the past, so compare it between modes, not against zero. Windows
thread CPU comes from `GetThreadTimes`, which is only accurate to a clock tick over one second.

## Model benchmark

`hosttiming_wait_bench.cpp` is a standalone model of the loop: 5, 10, 15 and 16.7 ms periodic events
plus guest timers scheduled 20 us to 3 ms ahead from another thread. It runs the hardware wait
alone (`hw-only`) and sleep-then-hardware-wait at each margin.

```sh
g++ -O2 -std=c++20 -pthread tools/nx2/hosttiming_wait_bench.cpp -o hosttiming_wait_bench
./hosttiming_wait_bench --seconds 5 --margins -1,0,100,250,500,1000
```

On the PC (clang-cl from a VS developer prompt, or the mingw g++):

```sh
clang-cl /O2 /std:c++20 /EHsc tools/nx2/hosttiming_wait_bench.cpp /Fe:hosttiming_wait_bench.exe
hosttiming_wait_bench.exe --seconds 5 --margins -1,0,100,250,500,1000,2000
```

It requests the maximum timer resolution like the emulator (`BENCH_DEFAULT_RESOLUTION=1` skips
that) and uses MWAITX when the CPU has it (`BENCH_NO_MWAITX=1` uses a pause loop). Pick the
smallest margin whose wake lateness p99 stays within a few microseconds of `hw-only` and whose
"sleeps past deadline" count is about zero.

## PC measurement

1. Build one package: `bash C:/lx/build-fix.sh egla91/hosttiming <name> "All - HostTiming wait" "EDEN_NX2_HOSTTIMING_STATS=1 EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545"`.
2. Run the benchmark above on the idle PC first, to choose the candidate margins.
3. For each of Bananza, OoT and MKW, load the usual save and stand in the usual scene. For each
   `margin_us` in -1, the chosen margin, half of it, and 0, in that order and then once more in reverse:
   - `--call '{"method":"host_timing","params":{"margin_us":M}}'` and wait 5 s.
   - `python -I thread_profile.py --seconds 15 --label M --out <game>-M.json`: HostTiming CPU %,
     its running/waiting split and context switches, plus process total CPU.
   - `frame_times {seconds: 15}`: fps and frame-time p90.
   - `log_tail {contains: "HostTiming wait"}`: the last 15 lines.
4. Accept a margin when fps and frame-time p90 stay within run-to-run noise of -1, wake late p99
   stays within about 10 us of -1, event late p99 does not grow, and HostTiming CPU drops well
   below 99%. Listen for audio crackle at each margin (audio events run every 5 ms).
5. Restore `margin_us` -1 and `stats` false when done.
