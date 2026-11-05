# Ordered descriptor-buffer encoding

This experiment moves only `vkGetDescriptorEXT` encoding for guest graphics and
compute descriptors from GPUBack to the existing VulkanWorker. It does not move
cache access, guest reads, ring allocation, pipeline compilation, or submission
decisions. It is not intended for nx2 without the experimental-switch removal
and validation that branch requires.

`EDEN_NX2_PERF_OPT=0` or
`EDEN_NX2_PERF_OFF=descriptor_workers` selects the original synchronous encoder.
`split_overlap` remains forcibly disabled. Known attached graphics debuggers and
the enabled RenderDoc hotkey also select the synchronous path, since capture
start/stop is not ordered through the scheduler.
Legacy builds retain the synchronous path because their buffer-cache deletion
policy includes immediate frees.

## Ownership and ordering

GPUBack resolves all descriptors and reserves the mapped ring range. The packet
owns copies of the entries and the layout's offsets, types, counts, and strides.
It captures the destination pointer and a reference to the device, not a pipeline,
cache, guest pointer, descriptor queue, or mutable last-payload vector. The worker
calls the unchanged `WriteDescriptorBuffer` before the subsequent bind and draw or
dispatch in the same FIFO. Reusing a graphics allocation does not queue another
write; the original write still precedes every consumer.

The existing scheduler queue is not bounded. A producer-only budget therefore
limits descriptor packet data to 1 MiB between worker drains, plus bounded command
and allocator overhead. This counts the inline packet, entry and binding copies,
and both verification buffers. An oversized packet takes the original path.
Exhausting the budget calls `WaitWorker`, not `Finish`; it does not wait for the
GPU unnecessarily. Completed packets are conservatively charged until a drain.
This adds no worker pool, atomic accounting, or worker-side packet allocation.

`DescriptorBufferRing::Allocate` still owns GPU reuse protection. Frame-slot reuse
waits for `frame_ticks[frame_index]`; exhaustion calls `Finish` before wrapping.
Both completion paths require the earlier FIFO submission, which follows the
packet's writes. After those GPU waits, `WaitDescriptorWrites` also drains any
pending CPU packets through the scheduler's execution mutex before mapped memory
is reused. A packet-budget drain alone never authorizes ring reuse. Allocation
generations and `TouchFrame` remain producer-owned.

## Lifetime and Vulkan requirements

The Vulkan command declaration for `vkGetDescriptorEXT` has no externally
synchronized parameters. The [threading rules][threading] permit concurrent host
calls unless external synchronization is required. The packet provides private
input data and a distinct output range. Other owner-side descriptor encodes can
run concurrently; nothing accesses command pools in this host call.

[The descriptor reference][descriptor] requires image views, samplers, and
address-backed buffers to survive their last dynamic descriptor access, not just
the encoding call. At this base:

- Rescaling and normal image deletion move old image views into the sentenced
  retirement ring. Rescaling retains the original and scaled images. Lazy view
  creation does not replace an existing Vulkan view.
- Sampler slots and sampler variants remain resident until cache destruction.
- Deleted images and native compute shadow views use GPU-tick retirement.
  Buffer and view retirement also has frame-based paths.

`RasterizerVulkan::TickFrame` now drains pending descriptor packets before any
pipeline, descriptor-ring, texture, or buffer cache retirement. This closes the
new host-encoding lifetime window without changing the existing GPU lifetime
policy. The sole `DeleteImage(..., true)` caller tests `scale_tick > frame_tick +
5`; its ordinary assignments are zero and `frame_tick + 1`, so this base has no
ordinary reachable in-frame immediate-delete path. If that policy changes, this
lifetime argument must be revisited.

The descriptor ring rejects non-host-coherent allocations. Encoding runs before
the queued `vkQueueSubmit` on the same worker thread. Vulkan's
[submission host-write ordering guarantee][host-writes] makes those writes
available to the GPU; moving the writer does not introduce a new device barrier
requirement.

Pipeline layouts are constructed synchronously before asynchronous build work.
Encoding does not inspect pipeline completion and is separate from bind lambdas
that can return early for failed builds. Existing build waits remain in order.
Frame-dump copies and their `Finish` boundaries remain in that FIFO. No cache or
capture callback runs inside a descriptor packet.

Normal rasterizer destruction already calls `WaitWorker` and `Finish` before
destroying the caches and ring. `CommandChunk` now also destroys placement-new
captures when abandoned and retains the not-yet-destroyed chain if execution
throws. Encoding itself is a void Vulkan query and allocates no packet storage on
the worker. Producer allocation failure cannot enqueue a partially built packet.
Existing uncaught worker exceptions, including device loss, remain fail-stop;
this change does not add device-loss recovery or claim graceful recovery from a
driver failure.

## Verification semantics

`EDEN_NX2_PERF_VERIFY=1` encodes the original entries and layout on the owner into
private reference storage, then encodes the copied packet on the worker. It does
not duplicate ring allocation, cache operations, binds, draws, or submissions.
Both scratch buffers start at zero. Comparison covers only descriptor spans,
excluding layout gaps and trailing set padding.

Descriptor bytes are opaque. The API does not provide a mask for implementation
padding or promise a canonical byte encoding. A byte difference therefore logs
an inconclusive opaque-byte warning once and copies the owner's reference spans
to the real ring. It does not label padding differences as an emulation bug,
discard differing bytes using an inferred mask, or assert semantic equivalence.
Equal bytes establish byte equality for that invocation, not correctness of the
resource bindings. Verification buffers are allocated on the owner and charged
to the same budget.

The focused tests use the production packet dispatcher, packet, budget, and
`CommandChunk` with a mutex/CV worker fixture. They exercise delayed execution,
source mutation and destruction, ordered consumption, host-memory reuse after a
drain, cancellation, exceptions, chunk overflow, bounded draining, branch on/off,
and verification padding behavior. They do not instantiate a Vulkan device or
replace a driver-backed ring/capture test.

After configuring the repository's CMake dependencies:

```sh
python3 tools/nx2/test_descriptor_packets.py --compiler clang++-20 --benchmark
python3 tools/nx2/test_descriptor_packets.py --compiler clang++-20 --sanitize address
python3 tools/nx2/test_descriptor_packets.py --compiler clang++-20 --sanitize thread
cmake --build build --target video_core --parallel 4
```

The synthetic benchmark times input preparation, copying, production command
recording, thread handoff, budget drains, encoding, and ordered consumption. The
encoder is deliberately not a Vulkan driver. These timings measure packet costs,
not GPUBack savings or FPS, and cannot establish a speedup in a game.

On the four-core cloud machine with Clang 20, the final rebased production
`video_core` target built successfully. All 10 focused tests passed 323 assertions
in release, ASan/UBSan, and TSan runs. The benchmark ran after compilation had
finished, with seven repetitions of 4,096 draws per row:

| Descriptors per draw | Direct median | Packet median |
|---|---:|---:|
| 8 | 0.939 ms | 6.003 ms |
| 64 | 6.404 ms | 13.101 ms |
| 256 | 23.425 ms | 36.980 ms |

Checksums matched in all 21 comparisons. The packet path is slower for this
synthetic encoder; these measurements provide no performance-win evidence. No
game, PC, capture, or driver-backed descriptor validation was run.

## Upstream and integration scope

The initial base was `e0ba310626` on `perf/oot-combined`; the final branch is rebased
onto `fc77a5d0f8`, including its owned asynchronous shader-build completion.
Upstream branches, all PR titles,
recent issues, descriptor-related history, and the diffs of PRs #262, #374 and
#639 were checked on 2026-11-05. #262 changes graphics descriptor scratch vectors
and texture-buffer binding; #374 changes descriptor-table reads; #639 protects
scheduler ownership. None implements this ordered encoding packet. Upstream was
not merged, as requested.

The only graphics pipeline implementation hunk replaces the synchronous encode
at line 737 of the updated base in `ConfigureDraw`, after successful ring reservation and
before cached allocation metadata is updated. The asynchronous pipeline-build
wait and completion sections are untouched.

[threading]: https://docs.vulkan.org/spec/latest/chapters/fundamentals.html#fundamentals-threadingbehavior
[descriptor]: https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDescriptorEXT.html
[host-writes]: https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-submission-host-writes
