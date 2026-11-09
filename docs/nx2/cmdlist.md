# Draw packets and command lists (stages 1 and 2 of the GPUBack command list)

GPUBack prepares every graphics draw in two stages. Stage A reads what the draw needs from the
Maxwell registers, dirty flags and guest memory as they are at the draw and writes it into a
`DrawPacket` (`src/video_core/renderer_vulkan/vk_draw_packet.h`). Stage B records the draw from
that packet. In stage 1 both run back to back on GPUBack in `RasterizerVulkan::PrepareDraw`;
there is no second thread and no switch back to the old path. Later stages move B behind a queue,
so B must not depend on the registers or dirty flags.

## What a packet holds

| Part | Contents |
|---|---|
| Header | Sequence number (one per prepared draw, for stage 4's ordering), pipeline, framebuffer, `DrawParams` or a copy of the indirect parameters, mesh and mesh emulation counts |
| Buffers (`VideoCommon::GraphicsBufferPacket`) | Index binding (when `Dirty::IndexBuffer` was set), index format, first, count and inline indexes; vertex streams to resolve or bind again with their strides; transform feedback bindings; storage and texture buffer bindings per stage, read from the constant buffers; indirect and count buffers; whether A uploaded buffer data |
| Descriptors | Where each stage's storage and texel buffer entries go in the descriptor queue's payload (A writes the other entries there itself); the rescaling and render area push constants |
| Flags | Transform feedback (ends the render pass and counts streamed bytes), sampled render target under a pending fragment barrier, rescaling, the zpass counter enable |
| Dynamic state | A presence mask and the values of each group whose dirty flag A took |

The dynamic state is a delta, not a register snapshot: a group is present only when A's
`Touch*` flag (or the state tracker's check) fired. `ReadDynamicGroup` in `vk_rasterizer.cpp`
computes each group's values; A and the verify both call it.

`DrawPacket::PayloadBytes` counts the bytes A wrote for the draw: the header fields the draw
uses (no indirect parameters for a direct draw, no mesh, index, transform feedback or indirect
fields where the draw has none), its lists by their used entries, and the dynamic state groups
present. Descriptor entries go to the descriptor queue, not the packet. EDEN_NX2_DRAW_PACKET_VERIFY
logs the average.

## Stage A

- Pipeline lookup (or the fast draw's reusable pipeline), texture resolve, texture memo, the
  render targets and the feedback loop check.
- Storage buffer addresses read from the constant buffers, texture buffer addresses from the
  resolved image views.
- `BufferCache::ReadGraphicsGeometry` reads the index, vertex, transform feedback and indirect
  buffers and takes `Dirty::IndexBuffer`, `Dirty::VertexBuffers` and `Dirty::VertexBuffer0+i`.
- The uniform buffers are resolved (`UpdateGraphicsUniformBuffers`, with its own retry loop) and
  bound: the stream buffer copies happen here, so they copy guest memory as it is at the draw.
- A acquires the descriptor payload and writes the entries in place. Per stage: the uniform buffer
  entries, room for the storage and texel buffer entries (`UpdateDescriptorQueue::Skip`, its range
  in the packet), the image entries (a delta draw copies its pipeline's kept image entries). Then
  the mesh emulation entries.
- The dynamic state flags are taken and their groups read into the packet. When binding the
  pipeline in B will make the state enables dirty again (`Scheduler::RefreshesStateEnable`, after
  a pipeline without extended dynamic state), A takes them too, so they are recorded with this
  draw as before the split.

## Stage B

`GraphicsPipeline::Record` and the rest of `PrepareDraw` use only the packet and state the caches
own:

1. Request the render pass end for transform feedback.
2. Resolve every buffer (retry loop): A's index and vertex bindings go to the buffer cache first;
   the storage, texture, transform feedback and indirect bindings are bound and resolved from the
   packet in the same pass. Then synchronize and upload, and bind the index, vertex, transform
   feedback and indirect buffers.
3. Per stage, write the storage and texel buffer entries (bound now) into the room A left
   (`BeginFill`/`EndFill`). Another count than A left fails the draw in `ConfigureDraw`, as a
   short payload did before.
4. The post-copy barrier, the fragment barrier check, the render pass, pipeline, descriptor set
   and push constants (`ConfigureDraw`).
5. The dynamic state records, the query counter enables and the draw command.

If B can't record the draw, it still records the dynamic state, so the flags A took are not lost.
If a submit happened since A (the scheduler's tick moved), the new command buffer has no dynamic
state, so B takes every group again before it records them.

## State B still shares

Stage 1 keeps these on GPUBack's shared state. Each is a stage 3/4 item.

- A reads scheduler state: the fast draw's `TakeReusableDraw`/`DeltaStillHolds` and
  `NoteResolvedDraw`, `HasFragmentBarrier`, `RefreshesStateEnable`, and the uniform stream
  buffer's reuse tick.
- After a submit between A and B, B reads the dynamic state from the registers and dirty flags.
- A acquires and writes the guest descriptor queue's payload, and B fills the room A left in it.
  Once A runs ahead, A must not acquire payload B hasn't recorded yet, and `TickFrame` must not
  reuse it before B has.
- B reads the page table for multi-range storage segments, and guest memory for uploads and uint8
  index widening.
- A writes the vertex repack layout into the buffer cache runtime and the mesh emulation buffers
  into the pipeline; B's `ConfigureDraw` reads the mesh group count and task buffers from the
  pipeline.
- Diagnostics between B's configure and its dynamic state (render state trace, frame dump, Ocarina
  tracking, indirect trace) read the registers and caches directly.

## Ordering changes against the old path

- A's recordings (texture uploads, uniform uploads) now come before B's buffer uploads.
- A resolves the uniform buffers before B resolves the others. If B's resolve merges a uniform
  buffer away, this draw's descriptor still names the old buffer; delayed destruction keeps it
  alive and its contents are those of the draw.
- `DeleteBuffer` and `RefreshRepackedVertexBuffers` no longer set the index and vertex dirty
  flags. They set resolve and rebind bits the buffer cache owns, so a deletion during a draw no
  longer records the vertex input again.

## Command lists (stage 2)

Stage A records into a command list, not the command buffer. `RasterizerVulkan::PrepareDraw`
calls `Scheduler::BeginCommandList` after `FlushWork`. From then on, `Scheduler::Record` and
`RecordWithUploadBuffer` put commands in the scheduler's one reusable list (a `CommandChunk`).
That covers the texture cache runtime's uploads, copies, clears and render target barriers, the
uniform buffer copies, render pass ends, and anything else A records. The texture cache itself is
unchanged.

`Scheduler::SpliceCommandList` runs right before `GraphicsPipeline::Record`, or at any early
return from the draw. It moves the list's commands, in order, to the end of the chunk. B's
commands follow them.

- **Empty lists:** splicing an empty list costs one flag store and one test. No list is
  allocated per draw.
- **Joins:** a submit (`Flush`, `Finish`) or a dispatch (`DispatchWork`, `WaitWorker`)
  during stage A splices the list first, and records its own commands into the chunk. A full
  list is spliced early too. Capturing continues after either.
- **State:** the scheduler's state (render pass, pipeline, deferred clears) still changes when A
  calls into it. On one thread this matches the order the commands land in, because nothing
  records between A and the splice. Stage 4 has to carry that state with the list.

`EDEN_NX2_CMDLIST_VERIFY=1` gives every recorded command a type tag and a hash of its captured
arguments. It numbers commands in the order they are recorded, which is the order the old direct
path put them in the command buffer, and checks that the command buffer receives them in that
order. A spliced command that comes out of the move as a different type also counts as a
difference. The first 16 differences get their own line. Every 10 s a `Command list verify`
line logs:
- commands checked and those that differ (must stay 0);
- draws, and how many of them had a list, with the commands those lists held;
- joins by a submit or dispatch, and by a full list.

## Verify

`EDEN_NX2_DRAW_PACKET_VERIFY=1` resolves each draw again from the live registers between A and B
and compares: draw or indirect parameters, the index binding, vertex bindings and strides,
storage buffer bindings, transform feedback and zpass enables, render area, framebuffer,
rescaling, and every dynamic state group the packet carries. It logs the first 16 differences and
every 10 s the draws checked, how many differed and the average packet size. Uniform and image
descriptors are not resolved again (that would upload uniforms and prepare images a second time);
the fast draw, texture memo and delta descriptor verifies cover them. In stage 1 every count must
stay 0, since nothing runs between A and B.
