# GPU backend profiling and parallel recording

## Phase 1: measure the existing pipeline

The current scheduler already overlaps resource resolution on GPUBack with Vulkan
recording on VulkanWorker. `draw record` measures the producer assembling callbacks,
not the driver's command encoding. The Ocarina sample has 1.7 ms/s in that zone
against 950 ms/s of back-thread CPU time. Star Fox has 5.6 ms/s against 650 ms/s.
Deleting those producer zones alone would save at most 0.18% and 0.86% of that CPU
work. Neither sample measures how much work another recording worker could remove.

Set `EDEN_NX2_GPU_PROFILE=1` and keep `EDEN_NX2_SPLIT_CHECK=1`. Every ten seconds,
the existing `per second` profile lines include fields prefixed `host` for the GPU
threads and VulkanWorker. The existing zone and `host_cpu_busy_ms_per_s` fields
remain available.

| Host phase | What it measures |
| --- | --- |
| `resolve` | Direct/indirect graphics preparation and compute dispatch preparation, excluding nested phases below. This includes pipeline lookup, resource/cache work, dynamic-state calculation and other host preparation. It is not a measure of pure cache lookup time. |
| `enqueue` | Construction and publication of scheduler commands inside `RecordWithUploadBuffer`. Captures evaluated before entering the function remain in preparation. |
| `encode` | Calls to `vkCmd*`, `vkBeginCommandBuffer` and `vkEndCommandBuffer` in the Vulkan wrappers, including upload, copy, barrier, query and compute commands. Forwarding helpers do not count twice. |
| `submit` | Calls to `vkQueueSubmit` and `vkQueueSubmit2`, excluding command encoding. |
| `worker other` | Callback execution and destruction excluding nested driver calls and explicit waits. This includes host descriptor updates, pipeline construction and any uninstrumented blocking inside callbacks. It is not parallelizable encoding. |
| `command pool` | Primary command-buffer acquisition and setup, excluding nested encoding and explicit waits. |
| `wait` | Scheduler worker idle waits, producer `WaitWorker`, and master-semaphore waits. Other mutex/driver waits remain in their containing phase. |

These are exclusive elapsed host times, not GPU execution times or CPU counters.
The separate per-thread CPU counter distinguishes runnable work from blocking.
The new phases overlap the old zone fields by design, so do not add the two
sets together. Within the host-phase fields, nested work is counted once. Work
outside the instrumented paths is not assigned to `resolve`; the old zone report
still shows methods, clears, decoding and other work. Sparse binding and presenting
are not queue-submit samples.

Report windows are per thread and may have different endpoints. Compare several
steady-state windows with the same scene and settings, after shader compilation.
Record frame-time distributions alongside them. Timing every driver call adds
overhead, so repeat the frame-time comparison with profiling unset. The checker
`python3 tools/nx2/check_host_gpu_profile.py` verifies the direct recording and
submission call sites are instrumented.

If encoding takes E ms/s, two perfectly balanced recording workers can remove at
most E/2 ms/s of that recording stage before coordination and repeated-state costs.
That is not an FPS prediction. Resolution and encoding already overlap, and more
recording workers cannot improve throughput when the ordered resolver is slower.
Submission, compute, copies, queries and uncertain dependencies will also remain
serial in the initial experiment, so the total encoding measurement is an upper
bound on the work eligible for graphics parallelism.

## Phase 2: immutable batches with serial replay

At checkpoint `d94ec5585d`, `EDEN_NX2_PARALLEL_RECORD=1` enables immutable command
capture with serial replay. Unset, `0`, or any other value keeps the original path.
That checkpoint tests correctness, not speed. The current implementation adds the
parallel encoder described below.

GPUBack remains the sole owner of guest resource resolution and buffer/texture
cache mutation. VulkanWorker still executes the existing callbacks in FIFO order,
including descriptor allocation, pipeline-build waits and query bookkeeping.
The immutable boundary follows those callbacks. Moving callbacks themselves to
workers would race shared allocators and borrowed descriptor storage.

`PassRecorder` captures resolved Vulkan values through the command-buffer wrapper.
`ResolvedCommand` is a closed variant. Pointer-bearing arguments become owned
arrays, including push-descriptor bytes whose size comes from the pipeline.
`ResolvedBatch` contains an immutable submission epoch, sequence number, compatible
render-pass inheritance, a complete state preamble and draw commands. It contains
no pointer to Maxwell state, guest memory or a cache object. Uploads, layout
transitions, barriers, compute and copies retain their original serial positions.

The state preamble is an exact history of state commands in the current primary
epoch. This preserves partial writes and pipeline/static-state interactions without
guessing at state invalidation rules. A batch covers up to 32 draws. History,
pending pass payload and cumulative replay cost are bounded; a budget overflow
selects serial recording for the remainder of the epoch. History is not a final
performance optimization: repeated state increases secondary encoding work.

The adapter delays the physical render-pass begin while it captures a candidate
pass. A late unsupported command materializes the prefix inline and keeps that
pass inline through its end. This emits the original clear/load operation once,
without an extra pass break. Queries, conditional rendering and transform feedback
prevent parallel capture while active. Unsupported persistent state and raw command
buffer escape disable capture for the entire primary epoch.

`WaitWorker` places a FIFO drain marker before waiting. An unfinished captured pass
becomes inline at that marker, so descriptor-arena reuse cannot wait for an end-pass
command that the blocked producer has not emitted. Frame retirement also drains the
worker before rotating payload arenas or releasing cache objects.

The submission epoch is the existing primary's signal value, checked against the
ordered submit callback. Batches never leave that submission or extend references
past its existing GPU retirement conditions. Cache-owned resources retain their
existing timeline/ring ownership; the batch does not pretend a raw handle is an
owning reference. The frame/drain gates enforce CPU recording completion before
retirement, and submission order remains unchanged.

With profiling enabled, `host resolved passes`, `host resolved batches`,
`host inline passes` and `host budget fallbacks` show the path taken. The parallel
encoder also reports `host parallel passes` and `host parallel batches`.

## Phase 3: parallel secondary recording

The same switch now attaches two recording workers to the immutable batch boundary.
Each worker owns its command pool and secondary buffers. VulkanWorker remains the
sole primary recorder and submitter; recording workers never call the scheduler,
read guest memory or mutate resource caches.

Passes with fewer than two batches stay inline because they offer no recording
parallelism. An eligible pass reserves all its secondary slots before dispatch. The queue holds
at most eight waiting jobs, with 256 retained secondary buffers across the two
workers. If a whole pass cannot fit, it replays inline instead of waiting for its
own unsubmitted GPU tick. Each slot carries the primary submission's explicit tick
and can be reused only after that tick completes on the GPU.

Workers can finish out of order. VulkanWorker waits for every job and executes the
secondary handles in original order, using one physical render-pass begin and end.
It restores the complete logical state onto the primary afterward because
`vkCmdExecuteCommands` invalidates primary command-buffer state. Callback side
effects are never repeated. Recording exceptions propagate rather than pretending
that a device loss was a successful serial fallback.

This first implementation records batches in parallel within one pass. It joins
them at the pass boundary, not at a later submission. Shutdown joins the recording
threads and waits for the last successful submission before destroying their pools.
Unsupported commands, active queries and exhausted budgets still use serial
recording. A low eligible-pass fraction or repeated-state overhead can make the
experiment slower; no game-speed improvement follows from compiling it.

## Verification

The full tests binary includes deterministic capture/replay and queue tests.
They cover owned payloads, overlapping state updates, late fallback, an open-pass
drain, reversed worker completion, capacity exhaustion, explicit tick retirement,
exception draining and shutdown.

The opt-in headless test uses synthetic shaders and offscreen readback. It compares
serial, captured and parallel pixels, requires four actual secondary buffers for
97 draws, exercises query/indirect/capacity fallback and repeats pool reuse after
retirement. A second pass in the same primary uses restored state without rebinding.
The test requires a Vulkan device and Khronos validation when enabled;
otherwise it reports a skip rather than claiming GPU coverage.

```sh
EDEN_TEST_VULKAN=1 \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
build/bin/tests
```

Use the installed ICD manifest on other systems. The production device-selection
policy is not part of this fixture; it creates a minimal graphics device so unrelated
optional extensions do not hide recorder validation failures.

Further resolution parallelism needs separate measurements. Guest reads, uploads,
hazards and cache mutation remain ordered; per-stage tasks would need snapshotted
inputs and must not mutate shared caches.

Upstream `nx2` was already contained in the phase-1 base `99ac3d5955`.
Eglantine PR #134 adds producer-side profiling but does not measure actual Vulkan
encoding or submission separately. Its diff and PR #593's split-drain changes
were inspected; neither implements immutable batches or parallel recording.
