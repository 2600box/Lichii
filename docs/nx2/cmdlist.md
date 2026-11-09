# Draw packets (stage 1 of the GPUBack command list)

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
| Descriptors | The uniform buffer entries and image entries of each stage and the mesh emulation entries, as A resolved them; the rescaling and render area push constants |
| Flags | Transform feedback (ends the render pass and counts streamed bytes), sampled render target under a pending fragment barrier, rescaling, the zpass counter enable |
| Dynamic state | A presence mask and the values of each group whose dirty flag A took |

The dynamic state is a delta, not a register snapshot: a group is present only when A's
`Touch*` flag (or the state tracker's check) fired. `ReadDynamicGroup` in `vk_rasterizer.cpp`
computes each group's values; A and the verify both call it.

`DrawPacket::PayloadBytes` counts what a packet carries, its lists by their used entries.
EDEN_NX2_DRAW_PACKET_VERIFY logs the average.

## Stage A

- Pipeline lookup (or the fast draw's reusable pipeline), texture resolve, texture memo, the
  render targets and the feedback loop check.
- Storage buffer addresses read from the constant buffers, texture buffer addresses from the
  resolved image views.
- `BufferCache::ReadGraphicsGeometry` reads the index, vertex, transform feedback and indirect
  buffers and takes `Dirty::IndexBuffer`, `Dirty::VertexBuffers` and `Dirty::VertexBuffer0+i`.
- The uniform buffers are resolved (`UpdateGraphicsUniformBuffers`, with its own retry loop) and
  bound: the stream buffer copies happen here, so they copy guest memory as it is at the draw. The
  descriptor queue's capture mode writes their entries, and the image entries, into the packet.
- The dynamic state flags are taken and their groups read into the packet.

## Stage B

`GraphicsPipeline::Record` and the rest of `PrepareDraw` use only the packet and state the caches
own:

1. Request the render pass end for transform feedback.
2. Apply the packet's bindings to the buffer cache, resolve every buffer (retry loop), synchronize
   and upload, and bind the index, vertex, transform feedback and indirect buffers.
3. Acquire the descriptor payload. Per stage: the packet's uniform entries, the storage and texel
   buffer entries (bound now), the packet's image entries. Then the mesh emulation entries.
4. The post-copy barrier, the fragment barrier check, the render pass, pipeline, descriptor set
   and push constants (`ConfigureDraw`).
5. The dynamic state records, the query counter enables and the draw command.

If B can't record the draw, it still records the dynamic state, so the flags A took are not lost.

## State B still shares

Stage 1 keeps these on GPUBack's shared state. Each is a stage 3/4 item.

- A reads scheduler state: the fast draw's `TakeReusableDraw`/`DeltaStillHolds` and
  `NoteResolvedDraw`, `HasFragmentBarrier`, and the uniform stream buffer's reuse tick.
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

## Verify

`EDEN_NX2_DRAW_PACKET_VERIFY=1` resolves each draw again from the live registers between A and B
and compares: draw or indirect parameters, the index binding, vertex bindings and strides,
storage buffer bindings, transform feedback and zpass enables, render area, framebuffer,
rescaling, and every dynamic state group the packet carries. It logs the first 16 differences and
every 10 s the draws checked, how many differed and the average packet size. Uniform and image
descriptors are not resolved again (that would upload uniforms and prepare images a second time);
the fast draw, texture memo and delta descriptor verifies cover them. In stage 1 every count must
stay 0, since nothing runs between A and B.
