# Stage probe (`EDEN_NX2_STAGE_PROBE`)

Test only (EGLA-91 stage 0, `handoffs/EGLA-91/cmdlist-plan.md`). It measures whether splitting the
thread that draws (the split's back) into stage A (methods, pipeline lookup, textures, render
targets, uniform copies) and stage B (buffer resolve, sync and binds, descriptors, render pass,
dynamic state, draw record) is worth building. Nothing is threaded. Measure without
`EDEN_NX2_GPU_PROFILE`, which inflates the times.

| Variable | Effect |
|---|---|
| `EDEN_NX2_STAGE_PROBE=1` | One `Stage probe per second (10 s): ...` line at Info (HW_GPU) every 10 s, from the thread that draws. Off: each hook tests one static bool. |
| `EDEN_NX2_STAGE_PROBE_SPIN=1` | Starts one unpinned thread that spins on the pause instruction for the whole run, with or without the probe: what one extra busy core costs the back. |

Both are read at startup and again when the GPU is created, so `nx2_switches.txt` can set them.

## How it measures

- **Clock.** rdtsc on x86, cntvct_el0 on arm64, calibrated to ns once over the first 10 s window.
  The line reports the probe's clock reads per second and an overhead estimate: reads times the
  measured cost of one region switch (clock read plus bookkeeping). Ring lookups are not in the
  estimate; their counts are in the line.
- **Busy, wait, draw, outside.** Every switch between these regions is timed: wait (the back
  waits for the front, the split wait; the idle hook's submit counts as busy), draw
  (`Draw`, `DrawIndirect`, `DrawTexture`), compute (`DispatchCompute`), clear and everything else
  ("outside draws": methods, macros, DMA, queries). Busy is wall time minus wait.
- **Stage B regions.** Timed only in 1 in 8 draws, picked at random, and scaled by the share of
  draw time those draws took. Regions nest: time always goes to the innermost one, so nothing
  is counted twice and the regions plus "rest of draw (A)" add up to the sampled draws' time.
  - index/vertex: `UpdateGraphicsBuffers` without the uniform and storage parts (index,
    vertex, transform feedback and indirect buffers)
  - storage: storage and texel buffer resolve
  - sync: `SynchronizeBuffer` (its walk and uploads) and the barrier after uploads; a sync
    inside a uniform bind stays uniform (A)
  - geometry bind: `BindHostGeometryBuffers`
  - stage bind: `BindHostStageBuffers` without the uniform buffers
  - descriptors: descriptor acquire, image descriptor push (and replay on delta draws), and
    `ConfigureDraw` (descriptor commit, pipeline bind) without the render pass request
  - render pass: `RequestRenderpass` (and `NoteRenderPassStores`)
  - dynamic state: `UpdateDynamicStates`
  - draw record: the draw function
  - uniform (A): uniform buffer resolve and binds with their copies
  - rest of draw (A): everything else in the draw (flush work, pipeline lookup, handles, image
    views, samplers, texture buffer binds, render targets, queries)

  "B share" is B over busy and over draw time; the third share adds compute and clears, which
  would be ordered packets on B.
- **Joins**, counted as if the stages existed. A ring keeps the device ranges stage B read in
  the last 64 draws (index buffer, bound vertex streams, storage and texel buffers), behind a
  64 KiB page filter. Joins are:
  - DMA launches (`MaxwellDMA::Launch`) whose destination overlaps the ring, plus DMA
    image<->buffer copies (`AccelerateDMA::DmaBufferImageCopy`, the only texture cache path into
    the buffer cache found) not already counted;
  - I2M / inline writes (`AccelerateInlineToMemory` and the block-linear upload) that overlap;
  - CB_DATA writes that overlap (per write batch: `ProcessCBMultiData` and the batched constant
    buffer words, not per method);
  - query, report and semaphore releases (`RasterizerVulkan::Query`, 4 or 16 bytes) that overlap;
  - flush reads: guest reads of GPU-written memory that ask the GPU thread for a flush
    (`GPU::OnCPURead`, not the in-flight ones) and buffer cache flushes over GPU-modified data
    (`RasterizerVulkan::FlushRegion`, any thread);
  - split waits: every drain or position wait of the front that did not return at once, by
    site, except ring full.

  Counted but not joins: compute dispatches and clears (ordered packets), texture buffer binds
  (packet data), DMA copies and inline writes the buffer cache made itself (ordered packets),
  ring-full waits, and all writes that miss the ring. "per 100 draws" divides by all draws.
- **Packet size**, per draw: 48 bytes of header (sequence, draw parameters, pipeline handle,
  flags) and the render target key (`sizeof(RenderTargets)`), 16 bytes per dirty vertex stream,
  dirty index buffer (plus inline indexes), storage and texel buffer binding, 8 per bound uniform
  buffer, 16 per image descriptor. Stage A's own command list (stage 2) is not estimated.

## The line

One line every 10 s, all values per second. Its format, with the unit test's numbers:

```
Stage probe per second (10 s): back busy 800.0 ms (split wait 200.0 ms) | draws 1000 (125 timed in detail), draw 500.0 ms, outside draws 300.0 ms, compute 0.0 ms (0), clear 0.0 ms (0) | B 100.0 ms = index/vertex 100.0 + storage 0.0 + sync 0.0 + geometry bind 0.0 + stage bind 0.0 + descriptors 0.0 + render pass 0.0 + dynamic state 0.0 + draw record 0.0 | uniform (A) 50.0 ms, rest of draw (A) 350.0 ms | B share 12.5% of busy, 20.0% of draw, 12.5% of busy with compute and clears | joins 1.00 per 100 draws (10): DMA 0 of 0 (0 overlap, 0 image<->buffer), I2M 0 of 0, CB_DATA 5 of 300, query 0 of 0, flush reads 0 (guest reads 0 flushing, 0 in flight; buffer flushes 0 of 0 over GPU-written data), split waits 5 (acquire 5) | not joins: compute 0, clears 0, texture buffer binds 0, DMA via buffer cache 0, I2M via buffer cache 0, texture->buffer cache calls 0, ring-full waits 7 | packet avg 400 B, max 900 B, 0.4 MB | ring 3 ranges, 0 reads, 0 scans (0 ranges compared), 0 overflows | probe 10000 timer calls, est. 0.10 ms (0.01% of busy)
```

The plan's gate: B at least 30% of busy and at most 2 joins per 100 draws, at the Bananza gate
and in MKW Free Roam. For the spinner A/B, compare frame rate and back busy with and without
`EDEN_NX2_STAGE_PROBE_SPIN=1` over alternating launches.

## Limits

- Only the thread that draws has data, made by its first draw; with `EDEN_NX2_SPLIT=0` that
  is the GPU thread, whose wait is only its queue wait.
- The sampled draws carry the probe's own switches, a few percent of their time, charged to
  whichever region is open.
- DMA destinations in block-linear layout use width x height x depth x bytes per pixel, without
  the block padding.
- Not counted: KeplerCompute's direct semaphore reduction write
  (`ProcessNativeReportSemaphore`), guest CPU writes, and guest reads that find nothing
  GPU-written.
- The buffer flush count checks `IsRegionGpuModified` under the lock `FlushRegion` already holds,
  only while the probe is on.
