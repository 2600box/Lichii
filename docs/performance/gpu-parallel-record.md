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

## Subsequent phases

Phase 1 does not enable `EDEN_NX2_PARALLEL_RECORD` or change scheduling. That
experiment switch belongs to the implementation of the following phases and must
remain off by default. No recording-speed improvement is claimed here.

1. Build immutable graphics batches after ordered resolution. Each batch needs a
   built pipeline, complete dynamic/geometry state, owned descriptor and push data,
   a compatible render-pass instance, explicit barriers and retained allocations.
   Handles and cache IDs alone are not resource ownership.
2. Use worker-owned secondary pools and preserve the existing scheduler worker as
   the sole primary recorder and submitter. Execute completed batches in guest
   order and retire resources on the submission timeline, not worker completion.
3. Keep unsupported draws, compute, copies, queries, conditional rendering,
   transform feedback and capture paths serial. A secondary-only render pass
   cannot accept an inline fallback draw. Pass transitions must preserve clear/load
   behavior, barriers and complete state rebinding.
4. Measure the result before adding resolution concurrency. Resolution must keep
   guest reads, uploads, hazards and cache mutation ordered. Per-stage tasks would
   need snapshotted inputs and must not mutate shared caches.

The existing arbitrary scheduler callbacks are not immutable batches. Graphics
callbacks can read borrowed descriptor payloads, mutate shared descriptor pools,
wait for pipelines and read settings. Query and timestamp callbacks mutate shared
query state. They cannot safely be handed to concurrent workers unchanged.

Upstream `nx2` was already contained in the phase-1 base `99ac3d5955`.
Eglantine PR #134 adds producer-side profiling but does not measure actual Vulkan
encoding or submission separately. Its diff and PR #593's split-drain changes
were inspected; neither implements immutable batches or parallel recording.
