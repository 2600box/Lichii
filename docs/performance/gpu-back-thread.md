# GPU back-thread work and parallel recording

This is an experiment and a design proposal, not a measured game-speed improvement.
The small experiment keeps all work on its current thread. A larger redesign needs
immutable draw packets before multiple threads can safely record Vulkan commands.

## What currently owns the work

References below describe base `8813b44794` and the layout-cache experiment.

| Work | State and ordering constraint | Decision |
| --- | --- | --- |
| Read handles | `vk_graphics_pipeline.cpp:414–510` also binds storage buffers. `shader_handle_program.h:18–34` reads current constant-buffer and storage words. `texture_cache.h:637–649` reads descriptors and updates cached sampler IDs. | Do not move the whole zone to the front or cache handles by pipeline alone. Guest writes can change the answer without changing the pipeline. |
| Bind stage buffers | `buffer_cache.h:492–499` dispatches uniform, storage, and texture-buffer binding. `vk_rasterizer.cpp:416–431` holds both cache mutexes around configuration. | Keep cache mutation, uploads, and host-resource allocation on the back. Moving the same locks to workers would serialize them again. |
| Method dispatch | `gpu_split.cpp:303–335` drains pending state before side-effectful methods. `maxwell_3d.cpp:925–942` initializes the front mirror only after draining the back. `FrontWrite` already removes eligible same-value writes at lines 965–972. | Do not distribute methods independently or treat the front mirror as a complete draw snapshot. Draw-manager state, bindings, and dirty flags still belong to the back. |
| Memory-layout lookup | `buffer_cache.h:2611–2633` queries the kind interval, then separately clamps native bindings to contiguous mapped pages. `memory_manager.cpp` protects kind-map reads and writes with `guard`. | Cache the last kind interval under the existing mutex. This does not cache guest bytes or change a dependency. |
| Vulkan recording | `vk_scheduler.h:164–184` appends commands to one scheduler chunk and checks its owner. | Workers must not call the current scheduler concurrently. They need separate command pools and resolved inputs. |

The writer barrier is not spare CPU time that can be removed. `dma_pusher.cpp:189`
and `:628` wait for prior writers before consuming memory. `gpu_split.cpp:222–223`
waits for the ordered queue position, which includes setup before the writer.
A front-thread handle read moved ahead of that position could read stale data.
`EDEN_NX2_SPLIT_CHECK=1` detects scheduler ownership mistakes, not all data races.

## The bounded experiment

`EDEN_NX2_SPLIT_REBALANCE=1` enables a last-interval cache for native memory-layout
queries. Unset or `0` uses the tree lookup. The value is read once per process.
This ports the interval-cache approach from `6242f664dafc219429cb4753f5db6675858e70c7`
on `perf/nx2-fast-draw`, with an experiment switch and diagnostics.

The cache stores interval bounds by value in each `RangeMap`. Every `Map` and
`Unmap` invalidates it. Hits and misses remain inside `MemoryManager::guard`, so a
mapping update cannot interleave with either lookup. No iterator survives a map
change. GPU writes, CPU writes, channel switches, and synchronization behavior
are unchanged because the cache contains mapping metadata, not memory contents.

This is separate from Jonas's `5f8cd77fe814b9f184f506e132d6e4f832f4acb1`.
That change caches `MaxContinuousRange` page-table walks and is assigned to the
vegetation port. This experiment only caches `GetMemoryLayoutSize` kind intervals.
Both can coexist, but their gains must not be counted twice from the same parent
profile zone. No second candidate is included: moving live handle reads or cache
binding is not a safe local change.

With `EDEN_NX2_GPU_PROFILE=1`, `memory layout` measures the lookup including its
mutex, and `layout lookups N (H cached)` reports rates. The scope is nested, so
time moves out of `read handles` or `compute` into `memory layout`. Compare their
sum when comparing against an older build. Profiling itself adds lookup overhead.

### Synthetic measurement

A cloud Clang 20 `-O3 -Wall -Wextra -Werror` probe made four million guarded
lookups among 4,096 mapped 64-KiB regions, with a 4-KiB gap between regions.
Seed 42 selected the next region every N lookups. Both modes produced checksum
`131086198400` in every workload. The table gives medians from five runs after
the build finished. These are synthetic results, not PC or game measurements.

| Lookups per selected region | Cache off, ns/query | Cache on, ns/query | Hit rate |
| --- | ---: | ---: | ---: |
| 1 | 111.38 | 113.48 | 0.024% |
| 8 | 64.42 | 23.17 | 87.503% |
| 64 | 61.84 | 10.95 | 98.438% |

Random addresses show no reliable gain and can regress. Clustered medians save
about 41–51 ns/query. Cloud timing varies: the eight-query workload ranged from
55.80–68.45 ns uncached and 22.80–25.61 ns cached. For each 100,000 queries/second,
the clustered medians remove only 4.1–5.1 ms of CPU work per second. At Ocarina's
recorded 44–50 ms presents this is about 0.18–0.25 ms/frame; at Star Fox's roughly
35 presents/second it is 0.12–0.15 ms/frame. These are conditional estimates at an assumed lookup rate,
not expected FPS gains. The new counters are needed to determine the real rate
and locality. The random case can regress instead.

The cache tests compare both paths against an independent byte-array mapping
oracle across 20,000 random remaps, splits, merges, and unmaps. Separate checks
cover disabled caching, hits inside an interval, invalid addresses, and invalidation.

Clang 20 built the complete `tests` and `yuzu-cmd` targets. All 655 test cases
passed with the experiment unset and again with the experiment, GPU profiler,
and split checks enabled. GCC 13 compiled the changed production and test units
with `-Werror` and no `-Wno-error` exceptions. The focused mapping tests passed
337,119 assertions under Clang, GCC, and Clang AddressSanitizer plus
UndefinedBehaviorSanitizer. These unit runs do not exercise a live split GPU
session or establish an FPS improvement.

## Why adding recording threads alone is insufficient

Ocarina build `fd5cd9b3a6`, Kokiri, has a ten-second profile ending at 151.167 s.
The back consumes 949.936 ms CPU/second. Its exclusive zones include 198.1 ms
read handles, 171.7 ms method dispatch, 138.4 ms other, 101.5 ms compute, and
82.8 ms stage-buffer binding. Nearby 600-present windows average 44.109 and
47.972 ms, not the 55.6 ms implied by an instantaneous 18 FPS display.
The same profile reports 243,000 KiB/second downloaded in 19,554 copies.

Star Fox baseline `27e10e043e` has a ten-second profile ending at 101.366 s.
Its back consumes 649.944 ms CPU/second, including 134.9 ms methods, 106.5 ms
other, 86.1 ms buffer updates, 79.3 ms stage-buffer binding, and 34.8 ms handles.
It spends 350.6 ms/second in split wait and downloads no buffers in that window.
These games and scenes have different bottlenecks.

The `draw record` zones are only 1.7 and 5.6 ms/second, respectively. Even deleting
those zones saves less than 0.09 ms/frame in Ocarina and about 0.16 ms/frame in
Star Fox. Those zones measure scheduler-side enqueue work, not the worker's Vulkan
calls. A trace must separate resource preparation, command encoding, and worker
execution before promising a gain from parallel recording.

## Proposed ownership change

Prefer an ordered resource resolver followed by parallel batch recording over
moving cache ownership to the front. The alternative, fully resolving draws on
the front, requires moving all relevant engine state and guest-write dependency
tracking there. It risks relocating the current bottleneck rather than reducing it.

The proposed caller flow is:

```text
front: decode and submit ordered engine operations
resolver: apply operations, resolve resources, publish immutable batches
workers: record each batch using a worker-owned Vulkan command pool
submitter: execute completed batches in original order
```

The resolver remains the sole owner of buffer-cache and texture-cache mutation,
resource uploads, dirty tracking, and hazard analysis. A `ResolvedBatch` contains
sequence numbers, immutable pipeline and draw state, descriptor data, retained
resource references, and explicit barriers. No packet holds a pointer to mutable
Maxwell registers, cache channel state, or guest memory. Resource versions remain
alive until the submission timeline retires them.

Workers record secondary command buffers from these packets. Each worker owns its
command pool and temporary storage. Descriptor allocation must use separate pools
or happen in the resolver. Each batch establishes its own required state instead
of inheriting another worker's mutable state cache. The submitter alone owns the
primary command buffer and timeline. It executes batches in guest order, preserving
render-pass compatibility, query ordering, image layouts, uploads, and barriers.
Initially, compute, copies, feedback queries, and uncertain dependencies stay in
serial batches. Recording in parallel does not imply executing dependent draws in
parallel on the GPU.

A bounded queue limits retained resources and stops the resolver from running
arbitrarily far ahead. Worker completion is not guest-memory writer completion.
Semaphore and syncpoint visibility must remain tied to the same ordered submission
and completion conditions as today. Unsupported paths keep the serial recorder.

No redesign code is implemented here. First measure worker encoding time and the
resolver fraction. With two recording workers, only half of the parallelizable
encoding time can disappear before coordination overhead. The current profiles
do not establish that fraction. Cache and download fixes should land and be
remeasured before deciding whether this redesign is worth its resource-lifetime
and synchronization complexity.

## Acceptance checks for the experiment

Compare identical warm scenes on one combined build with the switch off and on,
while holding the vegetation fixes, MaxContinuousRange cache, resolution, clocks,
and timer settings constant. Record frame-time distributions and the new lookup
rate, hit rate, and zone cost. Repeat with GPU profiling disabled to check observer
overhead. The owner or build coordinator controls launches; this task does not
build or launch a PC package.

Keep the experiment disabled if locality is poor or frame times do not improve.
Check Ocarina grass, vines, and ambient lighting as well as Star Fox rendering.
Remapping and channel changes must not alter bindings. A proven candidate must
lose its experiment switch before any merge to `nx2`; diagnostic counters may stay
off by default.
