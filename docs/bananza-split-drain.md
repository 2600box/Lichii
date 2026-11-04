# Bananza GPU thread split: per-entry drain

With the atomics fix (6fcce32e16), Bananza still drops to 20 to 30 fps. In those frames the game asks for swap interval 2 or 3 because `cpu_frame_wall_ms` reaches 65 to 124 ms. The owner's profile run (`EDEN_NX2_GPU_PROFILE`) shows where the GPU threads spend that time.

## Measurement

These are steady gameplay windows, 79 to 130 s. All values are per second of wall time.

| Thread | Work | Waiting on the other half |
|---|---|---|
| Front (GPU thread) | push 190-230 ms, macro 93-100 ms | split drain 610-680 ms, 114,000-139,000 waits |
| Back | about 730 ms, mostly update buffers 120-136 ms, method 108-152 ms, other 96-109 ms, bind stage buffers 74-84 ms | split wait 230-285 ms, 131,000-157,000 waits |

The back runs about 95,000 draws a second. The GPU itself is not the limit: the measured GPU time is about 8 ms per frame. The two halves of the split take turns more than 100,000 times a second instead of running at the same time.

## Cause

On Switch 2 titles, `DmaPusher::Step` called `GpuSplit::Drain()` before every command list entry. The front then waited until the back had run everything queued, and the back then waited for the next entry. Semaphore acquires (about 330 a second) and MME DMA reads (about 11 KiB a second) account for only a few hundred of the waits, so this drain accounts for nearly all of them.

Upstream nx2 has no such drain. It came from the fork's merge resolution (9a435b584ab, from 8cd5c6a44b and fae3bc64a9). Its purpose was to see a page that queued GPU work writes for the first time. Upstream covers that case without a full drain:

- `WaitForWriters` waits only for the compute and copy work sent so far, then the entry's pages are checked again.
- `FlushGpuWrittenCommands` drains only for an entry that is on GPU-written pages, before it flushes them.

## Change

- The per-entry drain is gone. `EDEN_NX2_FETCH_DRAIN=1` restores it, for an A/B comparison or if a command list problem appears.
- `FlushGpuWrittenCommands` drains again before it flushes, as upstream does.
- The continued macro and compute inline payload check runs on the back, as upstream does. The removed front-side check had set `gpu_written` for a dirty payload. The entry-level `IsGpuWrittenCommands` check already covers that, because the buffer cache marks every page it records as GPU-modified (`MarkWrittenBuffer`, buffer copies).

There is one difference from the forced drain. A draw that writes storage on a page for the first time is not a writer for `WaitForWriters`. If that page were a command list read right after the draw, the front could still take the submitted words. Upstream has the same rule: only compute and copy work writes command lists.

## Microbenchmark

`~/.capy/work/dkb-perf/gpu_split_bench.cpp` is a Catch2 case that drives the real `GpuSplit` ring. Each entry has 2.5 µs of front work and 5.9 µs of back work, which are the profile's per-entry costs. The case is not committed.

| Mode | ns per entry, three runs |
|---|---|
| Drain before every entry | 9,233 / 9,109 / 10,016 |
| Overlapped | 6,272 / 6,474 / 6,559 |

The same work takes about 31% less wall time when the halves overlap. That puts the GPU threads at about the back's own cost: about 730 ms of work a second, against about 1,000 ms of wall time now.

## Expected impact and profiling overhead

The GPU threads should go from about 42 ms per frame to about 30 ms at the measured draw load. That should keep Bananza at interval 2 (30 fps) where it now drops to interval 3. It will not reach 60 fps: the back's own work and guest core 5 (about 25 ms per frame) remain.

The next back-thread cost is about 530 scheduler finishes a second that wait about 100 ms in total, mostly for 36-210 MiB a second of buffer downloads. Upstream 660e5f0039 (fence submit batching) targets submit cost on the Vulkan worker, which runs at about 40% here, so it is not ported.

A profile scope costs 49 ns enabled and under 1 ns disabled, measured on the cloud VM. The back runs about 3 million scopes a second. Profiling therefore costs the back up to about 150 ms a second on that VM, probably less on the 5900X. Compare runs either both with `EDEN_NX2_GPU_PROFILE` or both without.
