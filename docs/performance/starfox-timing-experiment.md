# Star Fox timing experiment

This branch ports the Windows `Event::WaitFor` implementation and six regression
tests from [Eglantine #538](https://github.com/Djihads80/Eglantine/pull/538),
commit `a51de161ed2aa2264c1d1412f552c175b027904a`. Its unrelated sirit dependency
patch is not included. The base is `feat/debug-input` at `27e10e043e`; the Ampere
GPU-clock change from Lichii #114 is not included.

The experiment defaults to the existing Windows wait implementation.
`EDEN_TIMING_HIGH_RES=1` selects the upstream interruptible high-resolution timer
wait. A live debug-server call switches subsequent waits without restarting:

```json
{"method":"event_wait","params":{"high_resolution":true}}
{"method":"event_wait","params":{"high_resolution":false}}
```

Omit `high_resolution` to query the state. In-flight waits retain their original
implementation until they finish. Both policies receive `Event::Set` signals.
The switch applies to all `Common::Event::WaitFor` callers, not just HostTiming.
The live method is Windows-only. The flag, legacy implementation and live method
are experiment scaffolding and must not be proposed for merge.

Run the Event regression suite with `EDEN_TIMING_HIGH_RES=1`. The six upstream
tests are unchanged; two extra cases exercise switching policies on a reused
event and during a signalled wait. A Linux pass checks the portable path, not the
Windows timer implementation.

## Measurement

Use `EDEN_NX2_GPU_PROFILE=1 EDEN_DKB_FRAME_TRACE=1` in both states. Keep the game,
scene, settings and shader-cache state fixed. Measure the old, new, then old
policy in one session, allowing ten seconds after each change for the profiler's
window to settle. Compare HostTiming CPU time, guest presents and the frame
trace's fence, pacing, requested/actual sleep and interval fields. Lower CPU
usage alone is not a frame-rate gain. Check audio and input responsiveness too.

The baseline opening pause scene in Corneria took 28.47–29.09 ms per frame,
with 18.0–18.5 ms spent waiting for the emulated GPU fence. Windows thread CPU
accounting showed HostTiming near one full logical core. This motivates the
experiment; it does not prove timing waits cause the slow frames.

The GPU front thread's `writer wait` is a separate host-thread barrier.
`DmaPusher::Step` waits for the split back thread to execute through the last
compute/copy ring position before deciding whether a command page was written.
It does not mean the host GPU spent that duration writing a command list.
The queue includes all earlier graphics work, and `NoteWriter` currently marks
every compute/copy engine method, including register programming. Removing that
barrier without proving which pending work writes the next command range would
reintroduce stale command reads.

`EDEN_NX2_WRITER_TRACE=1` additionally attributes the front's barriers to the
last compute/copy method queued before each wait. Every ten seconds it reports
the eight methods with the greatest total wait, their wait counts and the ring
words still pending at entry. Engine 1 is compute; engine 4 is DMA. This measures
the whole ordered backlog before that method, not just the method's own work.
