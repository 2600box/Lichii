# Draw split probe (`EDEN_NX2_SPLIT_PROBE`)

The probe answers one question for EGLA-91 rank 4 (splitting the GPU back thread across cores):
how much of the back thread's busy time is per-draw work that could run on another core, given a
snapshot of the draw's inputs, and how much has to stay in guest order on the back thread. The
gate for building the split is a parallel share of at least 40% of the back thread's busy time in
Bananza.

The probe is a diagnostic. It is off unless `EDEN_NX2_SPLIT_PROBE=1` is set, and it is also off
while `EDEN_NX2_GPU_PROFILE` or `EDEN_NX2_STUTTER_TRACE` is set: it uses the profile's zones as its
segment boundaries, and their clock reads would land inside its stretches. While it is off, each zone costs one extra load of a flag.

## Run it on the PC

1. Build and package the branch. The env argument starts the debug server and the probe:

   ```sh
   bash C:/lx/build-fix.sh egla91/split-probe split-probe "Bananza - split probe" \
     "EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545 EDEN_NX2_SPLIT_PROBE=1" "*:Info"
   ```

2. Launch the shortcut `Lichii Debug - Bananza - split probe`, load the save with Continue and
   stand still at the Ingot Isle gate (the phase 0b spot).
3. After 30 s, read the probe lines and the frame rate from bash on the PC:

   ```sh
   PY=/c/Users/Zortos/AppData/Local/Programs/Python/Python314/python.exe
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
     --call '{"method":"log_tail","params":{"contains":"draw split probe","limit":20}}'
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
     --call '{"method":"frame_times","params":{"seconds":15}}'
   ```

   The lines are also in `eden_log.txt` next to the build.
4. Compare the frame rate with an unprobed run at the same spot (phase 0b: 14.5 to 14.8 fps). If
   the probe costs more than about 5%, treat its absolute numbers as inflated and use the ratios.

Take the median of at least 10 lines while standing still. Lines from loading or a scene change
carry structure changes and are not the steady state.

## Read the line

One line per second of wall time on the thread that draws (GPUBack with the split GPU thread):

```
draw split probe per second: serial 310.2 ms, parallel 205.7 ms, forced serial 1.3 ms (12 draws)
| 164000 draws in 517.2 ms, busy 845.0 ms (CPU 830.1 ms), parallel 24.3% of busy, 39.8% of draw
time | serial: flush work 43.0, uniforms 70.1, ... | parallel: draw setup 60.2, ...
| structure changes in parallel work: views 1.0 | in serial work: views 3.0, samplers 0.5
| probe clock reads 52.0 ms (7400000 at 7.0 ns)
```

(The numbers above show the format only; they are not a measurement.)

- **serial / parallel / forced serial:** milliseconds per second inside draws. Parallel excludes
  forced serial: the parallel work of draws that changed a cache's structure (see below), which a
  split would have to run on the serial path. The count in brackets is those draws per second.
- **draws in ... ms:** draws per second and the total time inside them (serial plus parallel,
  forced included).
- **busy:** wall time minus the time the thread waited for work (the split wait, the GPU
  thread's idle zones, or the worker's idle zone). Waits for the GPU or a presentation frame count
  as busy. CPU is the thread's CPU time over the same window. It counts the spinning
  part of the wait, so it can be higher than busy.
- **parallel % of busy:** the gate number. It is an upper bound, because each blind spot listed
  below moves work toward parallel, so reaching 40% is a necessary condition for the split, not
  proof that it pays. **% of draw time** compares with SerkiGohan's 40% for
  MKW.
- **serial: / parallel:** the time per kind of work, in ms per second. Forced time stays in its
  kind here.
- **structure changes:** buffers, images, image views, samplers and pipelines created per second,
  split by whether parallel or serial work made them.
- **probe clock reads:** the probe's own clock reads, which are part of the draw time above. The
  bookkeeping around each read adds a little more.

## How work is classified

Each stretch of a draw is classified by what it reads and writes. Serial work reads guest memory
or memory tracker state at draw time, or changes state that later commands read in order. The
guest rewrites constant buffers, handles and descriptors between draws, so these reads have to
happen at the draw's place in the command stream. Parallel work reads only the draw's registers
and cache lookups and writes only what the draw itself records, so a preparer could run it from a
snapshot.

| Kind | Class | Where it is timed | Why |
|---|---|---|---|
| flush work | serial | `FlushWork` zone | Hands chunks to the worker and invalidates four caches for the constant buffer writes since the last draw |
| uniforms | serial | `BindHostGraphicsUniformBuffers` | Copies uniform data from guest memory, which the next constant buffer update overwrites |
| buffer sync | serial | `SynchronizeBuffer` | Reads the memory tracker that guest writes and earlier invalidations change, and uploads |
| handles | serial | `ReadHandles`, `SyncDescriptors` zones | Texture handles, SSBO pointers and TSC entries read from guest memory |
| image views | serial | `FillImageViews` zone, `ResolveMemoizedTextures` | TIC entries and memo bytes read from guest memory; images checked for CPU writes |
| uploads | serial | `TextureUpload` zone | Writes images from guest memory |
| render targets | serial | `RenderTargets` zone | Render target images, framebuffers, render pass and barrier decisions, in stream order |
| locks | serial | around the cache mutexes in `PrepareDraw` | Excludes guest cores from the caches |
| queries | serial | query segment, transform feedback, counter enable in `PrepareDraw` | Query and feedback state is ordered |
| other | serial | any other zone inside a draw | Pipeline misses, shader capture, texture cache joins and blits, and waits for lists, the GPU, a presentation frame or a queue submit. The thread can do nothing else while it waits and a split would not remove the wait, so outside draws these count as busy too |
| draw setup | parallel | the rest of `Draw`, `DrawIndirect`, `PrepareDraw` | Draw parameters, fast-draw checks and vertex layout from registers |
| pipeline lookup | parallel | `PipelineLookup` zone | Key and hash lookup; a new pipeline forces the draw |
| configure | parallel | `Configure`, `TextureBuffers` zones | Per-draw bookkeeping and texel buffer bindings from resolved views |
| buffer resolve | parallel | `UpdateBuffers` zone | Addresses to cached buffers and index and vertex binds; a new buffer forces the draw |
| stage bindings | parallel | `BindStageBuffers` zone | Storage and texel buffer binds |
| descriptors | parallel | `DescriptorAcquire`, `PushDescriptors`, `ConfigureDraw` zones | Descriptor queue writes and binds from resolved ids |
| dynamic state | parallel | `DynamicState` zone | Registers to dynamic state commands |
| record | parallel | `DrawRecord` zone | The draw command |

Nested stretches count separately: a `SynchronizeBuffer` inside `UpdateBuffers` is buffer sync,
and the rest of `UpdateBuffers` is buffer resolve.

**Structure changes.** `CreateBuffer`, `InsertImage`, a new image view in `VisitImageView`, a
new sampler in `FindSampler` and a new pipeline key in `CurrentGraphicsPipelineSlowPath` change
a cache's structure. When parallel work makes one, its draw counts as forced serial, because a
preparer cannot insert into a shared cache. When serial work makes one, the draw itself is not
forced, but a split would have to wait for every draw still being prepared from the old cache
before the insert. The "in serial work" rate is how often that would happen.

## Blind spots

- **One class per stretch.** A stretch that mixes kinds gets the class of its main work. Handles
  include sampler lookups, image views include view lookups, uniforms include the bind, and the
  configure stretch includes `MemoizeTextures` on memo misses, which reads guest memory. Buffer
  resolve includes GPU-to-CPU address translation, which depends on the GPU memory map.
- **Compare with the previous draw.** Dynamic state, descriptors, vertex binds, the fast-draw
  delta and `CurrentGraphicsPipeline`'s transition list compare with the previous draw. They
  count as parallel on the assumption that each preparer keeps its own copy (SerkiGohan's mkw60
  B2 step 3 did that with 0 differences). The first draw of each batch then binds everything
  again, and the probe does not measure that.
- **Only structure changes force a draw.** LRU touches, buffer binding state, dirty flags, the
  descriptor queue, the scheduler's state tracker and memory tracker marks are state a preparer
  would keep per thread or log. Images and buffers deleted by invalidation, and GPU memory map
  changes, happen outside draws and are not counted.
- **Outside draws everything is serial.** Methods, clears, dispatches, DMA copies, fences and
  macros count only in busy time.
- **Wall time.** Preemption, page faults and lock spins count toward the stretch they hit. The
  split wait's idle hook counts as idle.
- **Probe cost.** One clock read per boundary, about 40 to 60 per draw (est.). Small stretches are
  inflated more than large ones.

## What a real split adds

The probe measures work that could move to another core. A real split also pays for:

- **The snapshot.** Registers, dirty masks and the bytes read from guest memory copied into a
  per-draw packet on the serial side.
- **Queues and wake-ups** between the serial stage and the preparers, and packet memory.
- **Ordered submission.** Each draw's commands have to reach the scheduler in guest order, with
  render pass changes and barriers between draws, so the recording side waits on a sequence
  number or merges per-preparer command streams in order.
- **Synchronisation.** A drain of in-flight preparers at every structure change made by serial
  work, the cache mutexes guest cores still take, and per-preparer descriptor queues and state
  trackers.
- **Fallbacks.** Forced draws run serially, after a precheck that decides which draws are safe.
- **The rest of the frame.** Methods, the front thread and the frames-ahead wait still bound the
  frame rate after the back thread gets faster.
