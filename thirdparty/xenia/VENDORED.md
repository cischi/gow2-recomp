# GPU and APU code vendored from xenia-canary

- Project: xenia-canary, https://github.com/xenia-canary/xenia-canary (fork
  of Xenia, branch `canary_experimental`)
- Commit: `e6cd2e4bc998609ca25e24a24c8e32b66c5908ad` (2026-10-20), pinned on
  2026-10-21.
  Until 2026-10-21 this was Xenia `xenia-project/xenia` @95a5c3e; the APU (XMA
  decoding) moved to canary on 2026-10-22
  and the old `thirdparty/xenia-apu/` folder no longer exists.
- License: BSD-3-Clause (`LICENSE` in this folder, a copy of canary's), except
  `third_party/` (own licenses, see below).
- The original file headers are intact.

The contents of `src/`, `third_party/` and `LICENSE` are produced by
`tools/vendor_xenia.py` and are never edited by hand:

```
python tools/vendor_xenia.py --xenia <xenia-canary checkout @e6cd2e4>           # regenerate
python tools/vendor_xenia.py --xenia <xenia-canary checkout @e6cd2e4> --check   # verify byte by byte
```

The script reads the files from the commit's git objects (not from the working
tree), copies only the paths in `manifest.txt` plus the sources of the shaders
included as bytecode (see the note after the file table), and applies the
patches in `patches/` in name order with `git apply -p1` (with
`core.autocrlf=false`: patched files stay LF like the blobs).
`.gitattributes` at the repository root marks `thirdparty/xenia/**` as `-text`,
so the bytes stay those of the commit even with `core.autocrlf`.

Every change to the vendored files is a patch in `patches/NNNN-<name>.patch`:
every hunk contains a `gow2:` comment and every patch has a row in the "Patch"
table with the reason (checked by `tests/test_vendor_xenia.py`).

## Files

Path here (relative to `thirdparty/xenia/`) and path in the xenia-canary
repository at the given commit: they are equal by construction.

| File | Origin in xenia-canary @e6cd2e4 | License |
|---|---|---|
| `src/xenia/apu/xma_context.cc` | `src/xenia/apu/xma_context.cc` | BSD-3-Clause |
| `src/xenia/apu/xma_context.h` | `src/xenia/apu/xma_context.h` | BSD-3-Clause |
| `src/xenia/apu/xma_context_new.cc` | `src/xenia/apu/xma_context_new.cc` | BSD-3-Clause |
| `src/xenia/apu/xma_context_new.h` | `src/xenia/apu/xma_context_new.h` | BSD-3-Clause |
| `src/xenia/apu/xma_context_old.cc` | `src/xenia/apu/xma_context_old.cc` | BSD-3-Clause |
| `src/xenia/apu/xma_context_old.h` | `src/xenia/apu/xma_context_old.h` | BSD-3-Clause |
| `src/xenia/apu/xma_helpers.h` | `src/xenia/apu/xma_helpers.h` | BSD-3-Clause |
| `src/xenia/apu/xma_register_file.cc` | `src/xenia/apu/xma_register_file.cc` | BSD-3-Clause |
| `src/xenia/apu/xma_register_file.h` | `src/xenia/apu/xma_register_file.h` | BSD-3-Clause |
| `src/xenia/apu/xma_register_table.inc` | `src/xenia/apu/xma_register_table.inc` | BSD-3-Clause |
| `src/xenia/base/assert.h` | `src/xenia/base/assert.h` | BSD-3-Clause |
| `src/xenia/base/bit_range.h` | `src/xenia/base/bit_range.h` | BSD-3-Clause |
| `src/xenia/base/bit_stream.cc` | `src/xenia/base/bit_stream.cc` | BSD-3-Clause |
| `src/xenia/base/bit_stream.h` | `src/xenia/base/bit_stream.h` | BSD-3-Clause |
| `src/xenia/base/byte_order.h` | `src/xenia/base/byte_order.h` | BSD-3-Clause |
| `src/xenia/base/byte_stream.cc` | `src/xenia/base/byte_stream.cc` | BSD-3-Clause |
| `src/xenia/base/byte_stream.h` | `src/xenia/base/byte_stream.h` | BSD-3-Clause |
| `src/xenia/base/hash.h` | `src/xenia/base/hash.h` | BSD-3-Clause |
| `src/xenia/base/literals.h` | `src/xenia/base/literals.h` | BSD-3-Clause |
| `src/xenia/base/mapped_memory.h` | `src/xenia/base/mapped_memory.h` | BSD-3-Clause |
| `src/xenia/base/math.h` | `src/xenia/base/math.h` | BSD-3-Clause |
| `src/xenia/base/memory.cc` | `src/xenia/base/memory.cc` | BSD-3-Clause |
| `src/xenia/base/memory.h` | `src/xenia/base/memory.h` | BSD-3-Clause |
| `src/xenia/base/mutex.h` | `src/xenia/base/mutex.h` | BSD-3-Clause |
| `src/xenia/base/platform.h` | `src/xenia/base/platform.h` | BSD-3-Clause |
| `src/xenia/base/platform_amd64.h` | `src/xenia/base/platform_amd64.h` | BSD-3-Clause |
| `src/xenia/base/platform_win.h` | `src/xenia/base/platform_win.h` | BSD-3-Clause |
| `src/xenia/base/ring_buffer.cc` | `src/xenia/base/ring_buffer.cc` | BSD-3-Clause |
| `src/xenia/base/ring_buffer.h` | `src/xenia/base/ring_buffer.h` | BSD-3-Clause |
| `src/xenia/base/string_buffer.cc` | `src/xenia/base/string_buffer.cc` | BSD-3-Clause |
| `src/xenia/base/string_buffer.h` | `src/xenia/base/string_buffer.h` | BSD-3-Clause |
| `src/xenia/base/xxhash.h` | `src/xenia/base/xxhash.h` | BSD-3-Clause |
| `src/xenia/gpu/command_processor.cc` | `src/xenia/gpu/command_processor.cc` | BSD-3-Clause |
| `src/xenia/gpu/command_processor.h` | `src/xenia/gpu/command_processor.h` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_command_processor.cc` | `src/xenia/gpu/d3d12/d3d12_command_processor.cc` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_command_processor.h` | `src/xenia/gpu/d3d12/d3d12_command_processor.h` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_primitive_processor.cc` | `src/xenia/gpu/d3d12/d3d12_primitive_processor.cc` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_primitive_processor.h` | `src/xenia/gpu/d3d12/d3d12_primitive_processor.h` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_query_pool.cc` | `src/xenia/gpu/d3d12/d3d12_query_pool.cc` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_query_pool.h` | `src/xenia/gpu/d3d12/d3d12_query_pool.h` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_render_target_cache.cc` | `src/xenia/gpu/d3d12/d3d12_render_target_cache.cc` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_render_target_cache.h` | `src/xenia/gpu/d3d12/d3d12_render_target_cache.h` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_shader.cc` | `src/xenia/gpu/d3d12/d3d12_shader.cc` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_shader.h` | `src/xenia/gpu/d3d12/d3d12_shader.h` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_shared_memory.cc` | `src/xenia/gpu/d3d12/d3d12_shared_memory.cc` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_shared_memory.h` | `src/xenia/gpu/d3d12/d3d12_shared_memory.h` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_texture_cache.cc` | `src/xenia/gpu/d3d12/d3d12_texture_cache.cc` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/d3d12_texture_cache.h` | `src/xenia/gpu/d3d12/d3d12_texture_cache.h` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/deferred_command_list.cc` | `src/xenia/gpu/d3d12/deferred_command_list.cc` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/deferred_command_list.h` | `src/xenia/gpu/d3d12/deferred_command_list.h` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/pipeline_cache.cc` | `src/xenia/gpu/d3d12/pipeline_cache.cc` | BSD-3-Clause |
| `src/xenia/gpu/d3d12/pipeline_cache.h` | `src/xenia/gpu/d3d12/pipeline_cache.h` | BSD-3-Clause |
| `src/xenia/gpu/draw_extent_estimator.cc` | `src/xenia/gpu/draw_extent_estimator.cc` | BSD-3-Clause |
| `src/xenia/gpu/draw_extent_estimator.h` | `src/xenia/gpu/draw_extent_estimator.h` | BSD-3-Clause |
| `src/xenia/gpu/draw_util.cc` | `src/xenia/gpu/draw_util.cc` | BSD-3-Clause |
| `src/xenia/gpu/draw_util.h` | `src/xenia/gpu/draw_util.h` | BSD-3-Clause |
| `src/xenia/gpu/dxbc.h` | `src/xenia/gpu/dxbc.h` | BSD-3-Clause |
| `src/xenia/gpu/dxbc_shader.cc` | `src/xenia/gpu/dxbc_shader.cc` | BSD-3-Clause |
| `src/xenia/gpu/dxbc_shader.h` | `src/xenia/gpu/dxbc_shader.h` | BSD-3-Clause |
| `src/xenia/gpu/dxbc_shader_translator.cc` | `src/xenia/gpu/dxbc_shader_translator.cc` | BSD-3-Clause |
| `src/xenia/gpu/dxbc_shader_translator.h` | `src/xenia/gpu/dxbc_shader_translator.h` | BSD-3-Clause |
| `src/xenia/gpu/dxbc_shader_translator_alu.cc` | `src/xenia/gpu/dxbc_shader_translator_alu.cc` | BSD-3-Clause |
| `src/xenia/gpu/dxbc_shader_translator_fetch.cc` | `src/xenia/gpu/dxbc_shader_translator_fetch.cc` | BSD-3-Clause |
| `src/xenia/gpu/dxbc_shader_translator_memexport.cc` | `src/xenia/gpu/dxbc_shader_translator_memexport.cc` | BSD-3-Clause |
| `src/xenia/gpu/dxbc_shader_translator_om.cc` | `src/xenia/gpu/dxbc_shader_translator_om.cc` | BSD-3-Clause |
| `src/xenia/gpu/gpu_flags.cc` | `src/xenia/gpu/gpu_flags.cc` | BSD-3-Clause |
| `src/xenia/gpu/gpu_flags.h` | `src/xenia/gpu/gpu_flags.h` | BSD-3-Clause |
| `src/xenia/gpu/packet_disassembler.cc` | `src/xenia/gpu/packet_disassembler.cc` | BSD-3-Clause |
| `src/xenia/gpu/packet_disassembler.h` | `src/xenia/gpu/packet_disassembler.h` | BSD-3-Clause |
| `src/xenia/gpu/pipeline_util.h` | `src/xenia/gpu/pipeline_util.h` | BSD-3-Clause |
| `src/xenia/gpu/pm4_command_processor_declare.h` | `src/xenia/gpu/pm4_command_processor_declare.h` | BSD-3-Clause |
| `src/xenia/gpu/pm4_command_processor_implement.h` | `src/xenia/gpu/pm4_command_processor_implement.h` | BSD-3-Clause |
| `src/xenia/gpu/primitive_processor.cc` | `src/xenia/gpu/primitive_processor.cc` | BSD-3-Clause |
| `src/xenia/gpu/primitive_processor.h` | `src/xenia/gpu/primitive_processor.h` | BSD-3-Clause |
| `src/xenia/gpu/register_file.cc` | `src/xenia/gpu/register_file.cc` | BSD-3-Clause |
| `src/xenia/gpu/register_file.h` | `src/xenia/gpu/register_file.h` | BSD-3-Clause |
| `src/xenia/gpu/register_table.inc` | `src/xenia/gpu/register_table.inc` | BSD-3-Clause |
| `src/xenia/gpu/registers.cc` | `src/xenia/gpu/registers.cc` | BSD-3-Clause |
| `src/xenia/gpu/registers.h` | `src/xenia/gpu/registers.h` | BSD-3-Clause |
| `src/xenia/gpu/render_target_cache.cc` | `src/xenia/gpu/render_target_cache.cc` | BSD-3-Clause |
| `src/xenia/gpu/render_target_cache.h` | `src/xenia/gpu/render_target_cache.h` | BSD-3-Clause |
| `src/xenia/gpu/sampler_info.cc` | `src/xenia/gpu/sampler_info.cc` | BSD-3-Clause |
| `src/xenia/gpu/sampler_info.h` | `src/xenia/gpu/sampler_info.h` | BSD-3-Clause |
| `src/xenia/gpu/shader.cc` | `src/xenia/gpu/shader.cc` | BSD-3-Clause |
| `src/xenia/gpu/shader.h` | `src/xenia/gpu/shader.h` | BSD-3-Clause |
| `src/xenia/gpu/shader_interpreter.cc` | `src/xenia/gpu/shader_interpreter.cc` | BSD-3-Clause |
| `src/xenia/gpu/shader_interpreter.h` | `src/xenia/gpu/shader_interpreter.h` | BSD-3-Clause |
| `src/xenia/gpu/shader_storage.h` | `src/xenia/gpu/shader_storage.h` | BSD-3-Clause |
| `src/xenia/gpu/shader_translator.cc` | `src/xenia/gpu/shader_translator.cc` | BSD-3-Clause |
| `src/xenia/gpu/shader_translator.h` | `src/xenia/gpu/shader_translator.h` | BSD-3-Clause |
| `src/xenia/gpu/shader_translator_disasm.cc` | `src/xenia/gpu/shader_translator_disasm.cc` | BSD-3-Clause |
| `src/xenia/gpu/shared_memory.cc` | `src/xenia/gpu/shared_memory.cc` | BSD-3-Clause |
| `src/xenia/gpu/shared_memory.h` | `src/xenia/gpu/shared_memory.h` | BSD-3-Clause |
| `src/xenia/gpu/texture_address.h` | `src/xenia/gpu/texture_address.h` | BSD-3-Clause |
| `src/xenia/gpu/texture_cache.cc` | `src/xenia/gpu/texture_cache.cc` | BSD-3-Clause |
| `src/xenia/gpu/texture_cache.h` | `src/xenia/gpu/texture_cache.h` | BSD-3-Clause |
| `src/xenia/gpu/texture_extent.cc` | `src/xenia/gpu/texture_extent.cc` | BSD-3-Clause |
| `src/xenia/gpu/texture_info.cc` | `src/xenia/gpu/texture_info.cc` | BSD-3-Clause |
| `src/xenia/gpu/texture_info.h` | `src/xenia/gpu/texture_info.h` | BSD-3-Clause |
| `src/xenia/gpu/texture_info_formats.cc` | `src/xenia/gpu/texture_info_formats.cc` | BSD-3-Clause |
| `src/xenia/gpu/texture_info_formats.inl` | `src/xenia/gpu/texture_info_formats.inl` | BSD-3-Clause |
| `src/xenia/gpu/texture_util.cc` | `src/xenia/gpu/texture_util.cc` | BSD-3-Clause |
| `src/xenia/gpu/texture_util.h` | `src/xenia/gpu/texture_util.h` | BSD-3-Clause |
| `src/xenia/gpu/trace_protocol.h` | `src/xenia/gpu/trace_protocol.h` | BSD-3-Clause |
| `src/xenia/gpu/trace_reader.h` | `src/xenia/gpu/trace_reader.h` | BSD-3-Clause |
| `src/xenia/gpu/ucode.cc` | `src/xenia/gpu/ucode.cc` | BSD-3-Clause |
| `src/xenia/gpu/ucode.h` | `src/xenia/gpu/ucode.h` | BSD-3-Clause |
| `src/xenia/gpu/xenos.cc` | `src/xenia/gpu/xenos.cc` | BSD-3-Clause |
| `src/xenia/gpu/xenos.h` | `src/xenia/gpu/xenos.h` | BSD-3-Clause |
| `src/xenia/gpu/xenos_zpd_report.h` | `src/xenia/gpu/xenos_zpd_report.h` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_api.h` | `src/xenia/ui/d3d12/d3d12_api.h` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_cpu_descriptor_pool.cc` | `src/xenia/ui/d3d12/d3d12_cpu_descriptor_pool.cc` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_cpu_descriptor_pool.h` | `src/xenia/ui/d3d12/d3d12_cpu_descriptor_pool.h` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_descriptor_heap_pool.cc` | `src/xenia/ui/d3d12/d3d12_descriptor_heap_pool.cc` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_descriptor_heap_pool.h` | `src/xenia/ui/d3d12/d3d12_descriptor_heap_pool.h` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_gpu_completion_timeline.cc` | `src/xenia/ui/d3d12/d3d12_gpu_completion_timeline.cc` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_gpu_completion_timeline.h` | `src/xenia/ui/d3d12/d3d12_gpu_completion_timeline.h` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_provider.cc` | `src/xenia/ui/d3d12/d3d12_provider.cc` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_provider.h` | `src/xenia/ui/d3d12/d3d12_provider.h` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_upload_buffer_pool.cc` | `src/xenia/ui/d3d12/d3d12_upload_buffer_pool.cc` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_upload_buffer_pool.h` | `src/xenia/ui/d3d12/d3d12_upload_buffer_pool.h` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_util.cc` | `src/xenia/ui/d3d12/d3d12_util.cc` | BSD-3-Clause |
| `src/xenia/ui/d3d12/d3d12_util.h` | `src/xenia/ui/d3d12/d3d12_util.h` | BSD-3-Clause |
| `src/xenia/ui/dxgi_include_win.h` | `src/xenia/ui/dxgi_include_win.h` | BSD-3-Clause |
| `src/xenia/ui/gpu_completion_timeline.h` | `src/xenia/ui/gpu_completion_timeline.h` | BSD-3-Clause |
| `src/xenia/ui/graphics_upload_buffer_pool.cc` | `src/xenia/ui/graphics_upload_buffer_pool.cc` | BSD-3-Clause |
| `src/xenia/ui/graphics_upload_buffer_pool.h` | `src/xenia/ui/graphics_upload_buffer_pool.h` | BSD-3-Clause |
| `src/xenia/ui/graphics_util.cc` | `src/xenia/ui/graphics_util.cc` | BSD-3-Clause |
| `src/xenia/ui/graphics_util.h` | `src/xenia/ui/graphics_util.h` | BSD-3-Clause |
| `third_party/FidelityFX-CAS/LICENSE.txt` | submodule `third_party/FidelityFX-CAS` @9fabcc9 | MIT (AMD) |
| `third_party/dxbc/DXBCChecksum.cpp` | `third_party/dxbc/DXBCChecksum.cpp` | AMD (MD5 "free for all", see below) |
| `third_party/dxbc/DXBCChecksum.h` | `third_party/dxbc/DXBCChecksum.h` | AMD (MD5 "free for all", see below) |
| `third_party/fxaa/LICENSE` | `third_party/fxaa/LICENSE` | BSD-3-Clause (NVIDIA) |

Shader sources derived from the includes: for every bytecode header
`xenia/<gpu|ui>/shaders/bytecode/d3d12_5_1/<id>.h` included by the copied files
(and by `runtime/gpu/*.cpp`, gow2's presenter), `tools/vendor_xenia.py`
(`shader_sources`) copies the source `<base>.<stage>.xesl` or `.hlsl` from
`src/xenia/<gpu|ui>/shaders/` and, transitively, the files it includes
(`*.xesli`, `*.hlsli`, `third_party/fxaa/FXAA3_11.h`, the headers of
`third_party/FidelityFX-CAS/ffx-cas/` read from the submodule at the gitlink
commit). They are not in the manifest. The bytecode headers are not vendored:
the build compiles them with fxc (`cmake/XeniaShaders.cmake`, same flags as
xenia-build; `tests/test_xenia_shaders.py` checks that the bytes are those
committed in Xenia @95a5c3e). License: Xenia's, except the files in
`third_party/` (below).

## Patches

| Patch | Files | Reason |
|---|---|---|
| `0001` | (dropped) | Dropped with canary: `CP_RB_WPTR` (0x01C5) is already in canary's `src/xenia/gpu/register_table.inc`. |
| `0002` | (dropped) | Dropped with canary: Xenia's code builds as C++20 and uses the STL's `std::endian`, no `byte_order.h` polyfill. |
| `0003-gow2-register-ring.patch` | `src/xenia/gpu/command_processor.cc`, `src/xenia/gpu/d3d12/d3d12_command_processor.cc` | Ring/register bridge with the guest region 0x7FC80000 (`GraphicsSystem` shim). `WorkerThreadMain` reads the write pointer from `gow2_PollWritePointer` (the CPU writes `CP_RB_WPTR` to memory, there is no MMIO) and after every `ExecutePrimaryBuffer` copies the read pointer into the region's `CP_RB_RPTR`. Every command processor register write also goes into the region (`gow2_MirrorRegister`): in canary in `CommandProcessor::WriteRegister`, `D3D12CommandProcessor::WriteRegisterForceinline`, `WritePossiblySpecialRegistersFromMem` and in the ordinary registers of `WriteRegisterRangeFromMem_WithKnownBound` (not the shader, fetch, bool and loop constants, which the CPU does not read back). On Xenia @95a5c3e it also added `CP_RB_RPTR` to `register_table.inc`: canary already has it. Tests: `xenia_cp: CP_RB_WPTR in the guest region ...`, `ring buffer wrap-around`, `SCRATCH_REG0 ...`, `d3d12_cp: ...`. |
| `0004-gow2-wait-reg-mem.patch` | `src/xenia/gpu/pm4_command_processor_implement.h` | `WAIT_REG_MEM` on a register reads `gow2_ReadRegisterForWait` (guest region; `COHER_STATUS_HOST` from the RegisterFile after `MakeCoherent`); an index outside the RegisterFile does not read out of the array; long waits sleep at most 1 ms per iteration and short ones yield the processor and check `worker_running_` (in canary they spin), so `Shutdown` does not hang. Tests: `xenia_cp: WAIT_REG_MEM on SCRATCH_REG3 ...`, `WAIT_REG_MEM on COHER_STATUS_HOST ...`, `d3d12_cp: ... Shutdown within 2 s`. |
| `0005-gow2-draw-counters.patch` | `src/xenia/gpu/command_processor.cc`, `src/xenia/gpu/d3d12/d3d12_command_processor.cc`, `src/xenia/gpu/pm4_command_processor_implement.h` | Summary counters: `gow2::hooks::OnDraw(draw_succeeded)` at the end of `ExecutePacketType3Draw` (`gpu_draws`, `gpu_draws_skipped`) and `gow2::hooks::OnUnknownPacket(opcode)` in `HitUnimplementedOpcode` (`gpu_unknown_packet`). In canary the PM4 code lives in `pm4_command_processor_implement.h`, included at the bottom of the two `.cc` files: `gow2_hooks.h` is included there. Tests: `xenia_cp: DRAW_INDX with a failing IssueDraw ...`, `xenia_cp: opcode 0x7E ...`. |
| `0006-gow2-provider-without-presenter.patch` | `src/xenia/ui/d3d12/d3d12_provider.{h,cc}` | Ported to canary when moving to it (needed to build). `D3D12Provider` without `CreatePresenter`/`CreateImmediateDrawer` (Xenia's presenter and immediate drawer are not vendored; the `GraphicsProvider` shim does not declare them; presentation lives in runtime/gpu), without `EnableIncreaseBasePriorityPrivilege` (process token: `d3d12_queue_priority=2` counts as 1, high priority) and without loading `dxilconv.dll`/`dxcompiler.dll` (debug DXIL disassembly only: `DxbcConverterCreateInstance`/`DxcCreateInstance` return `E_NOINTERFACE`). `dxgi.dll`, `D3D12.dll` and `D3DCompiler_47.dll` (optional) remain. Test: `d3d12_cp: D3D12Provider::Create with device and direct queue`. |
| `0007-gow2-xma-counters.patch` | `src/xenia/apu/xma_context_new.cc`, `src/xenia/apu/xma_context_old.cc` | Summary counters used by the audio milestone: `gow2::hooks::OnXmaFrameDecoded()` for every frame received from `avcodec_receive_frame` and `OnXmaDecodeError(id)` on `avcodec_send_packet`/`avcodec_receive_frame` failures (`xma_frames_decoded`, `xma_decode_errors`). `AVERROR(EAGAIN)` does not count: in `XmaContextNew` it is the normal "needs more input" path. It was 0001 of `thirdparty/xenia-apu` on Xenia @95a5c3e. Tests: `xma_hooks`, audio milestone. |
| `0008-gow2-xma-empty-output.patch` | `src/xenia/apu/xma_context_new.cc`, `src/xenia/apu/xma_context_old.cc` | With `output_buffer_valid = 1` and `output_buffer_block_count = 0`, `XmaContextNew::Work` builds a zero-capacity `RingBuffer` (`set_read_offset` does `% 0`, crash) before its own check: early exit with a counted error. `XmaContextOld` already exits earlier: only the counter is added. It was 0002 of `thirdparty/xenia-apu`. Test: `xma_decoder: hostile contexts ...`. |
| `0009-gow2-msaa-2x-samples.patch` | `src/xenia/gpu/draw_util.{h,cc}`, `src/xenia/gpu/d3d12/d3d12_command_processor.cc` | 2x MSAA sample 0 as on the console on the rtv path: 2x draws are shifted by (0.25 - 0.25/sx, -0.25 - 0.25/sy) guest px (0, -0.5 at 1x), so the depth resolved from sample 0 matches the console (no black triangles on the characters). On canary: bit `msaa_2x_sample_0_to_console` of `GetViewportInfoArgs`, set by `IssueDraw` on the rtv path without Xenos positions. Canary's Xenos positions (`xenos_sample_positions`, `GetHostSampleXenosPositions`) are not enough (2026-10-21, RTX 4080): with the default (off on the rtv path) all 3844 pixels of the 2x depth are wrong; with them on, the plain 2x depth is right but after a transfer from 1x 1922 pixels stay wrong. Tests: `d3d12_msaa: 2x depth from sample 0 at the pixel center`, `... after a transfer from 1x`, `draw_util: shift of 2x draws toward the console's sample 0, per scale`. |
| `0010-gow2-replacement-shaders.patch` | `src/xenia/gpu/shader.h`, `src/xenia/gpu/d3d12/pipeline_cache.{h,cc}` | Applies unchanged on canary (hook in `PipelineCache::TranslateAnalyzedShader`). Runtime replacements by hash (replacement shaders spec). `Shader::Translation::ReplaceTranslatedBinary` replaces the translated DXBC; `SetTranslatedPixelShaderHook` sets an atomic function pointer that `PipelineCache::TranslateAnalyzedShader` calls after every successful pixel shader translation (with ROV path and bindless resources), before the binding layouts and the dump: root signature, bindings and PSO stay those of Xenia's translation. The patch does not know the registry: the runtime (`runtime/gpu/shader_overrides.cpp`) checks the layout and replaces. Tests: `replacement shader: DOF downsample equals the formula on synthetic textures`, `replacement shader: different layout, Xenia's translation stays`. |
| `0011` | (dropped) | Dropped with canary: canary's `TextureCache::ScaledResolveGlobalWatchCallback` already masks the blocks outside the written range ("Pre-mask to only process blocks within the write range"), the same fix. |
| `0012-gow2-command-processor-subclass.patch` | `src/xenia/gpu/d3d12/d3d12_command_processor.h`, `src/xenia/gpu/pm4_command_processor_implement.h` | In canary `D3D12CommandProcessor` is `final` and the PM4 code (`pm4_command_processor_implement.h`, included in the class) calls `COMMAND_PROCESSOR::IssueSwap` and `COMMAND_PROCESSOR::IssueDraw` without virtual dispatch. `runtime/gpu/d3d12_backend.cpp` derives `Gow2D3D12CommandProcessor` from it (presents on every `XE_SWAP`, counts resolves in `IssueCopy`): no `final` and virtual calls to `IssueSwap` and `IssueDraw` (the tests' command processor, `tests/gpu/null_command_processor.h`, also overrides `IssueDraw`). Tests: `d3d12_backend: VdSwap ...`, `xenia_cp: DRAW_INDX ...`. |
| `0013-gow2-upscaler.patch` | `src/xenia/gpu/draw_util.{h,cc}`, `src/xenia/gpu/d3d12/d3d12_command_processor.{h,cc}`, `src/xenia/gpu/d3d12/d3d12_render_target_cache.{h,cc}`, `src/xenia/gpu/d3d12/d3d12_texture_cache.{h,cc}` | Temporal upscaler. Jitter: `SetGow2Jitter(x, y, surface_pitch)` (guest px) applied by `IssueDraw` to draws with an active depth test on surfaces with the scene's pitch (2x MSAA geometry and 1x lights), except depth restores (pixel shader writing depth with an ALWAYS test), in `GetViewportInfoArgs::gow2_jitter_xy` added to `offset_add_xy` after 0009 and after the Xenos positions correction (`xenos_sample_positions`, which only recognizes the exact sample fractions); a serial number in `padding_set_to_0` invalidates the cached viewport when the jitter changes; jitter (0, 0) = unchanged behavior. Copies for the capture after the uber post: `Gow2CopyRect` (rectangle into a texture, source state restored), `D3D12RenderTargetCache::Gow2ColorTarget0`/`Gow2CopyColorTarget0` (color render target 0 of the last Update, RTV path only), `D3D12TextureCache::Gow2ActiveTexture`/`Gow2CopyActiveTexture` (host texture of the active binding). Tests: `draw_util: upscaler jitter in guest px, per scale`, `draw_util: upscaler jitter with the Xenos sample positions`. |
| `0014-gow2-mip-bias.patch` | `src/xenia/gpu/dxbc_shader_translator.{h,cc}`, `src/xenia/gpu/dxbc_shader_translator_fetch.cc`, `src/xenia/gpu/shaders/xenos_draw.hlsli`, `src/xenia/gpu/d3d12/d3d12_command_processor.{h,cc}` | Negative mip bias with the upscaler. System constant `xe_gow2_mip_bias` in the slot that was padding after `param_gen_window_offset` (layout and size unchanged), added to the LOD bias of fetches with computed LOD in pixel shaders (not explicit LOD, not `kBaseMap`, not VS). `SetGow2MipBias(bias)`; `UpdateSystemConstantValues` writes it only for the scene draws of 0013 (same predicate as the jitter, now in `gow2_scene_draw_`), otherwise 0. Test: `xenia_core: upscaler mip bias in the system constants`. |
| `0016-gow2-frequent-submissions.patch` | `src/xenia/gpu/d3d12/d3d12_command_processor.{h,cc}` | Cvar `d3d12_submit_every_draws` (default 0 = Xenia unchanged; gow2 sets it to 100 in `main.cpp`): `IssueDraw` closes the submission after N draws in the open submission (`gow2_draws_in_submission_`, reset in `EndSubmission`) if `CanEndSubmissionImmediately` and only with a 1x1 resolution scale. With strict occlusion queries the game waits every 2 frames for the result of a query just sent: the GPU starts earlier and the wait shortens (1x: from ~115 to ~144 fps). At 2x/3x the GPU is full (no gain) and splitting ZPD reports brings back the blue armor (cause in the counts at scale > 1 not found). Tests: `d3d12_draw: one submission per draw, same result`, `d3d12_draw: ZPD report with one submission per draw`, `d3d12_backend: the Xenia cvars set by gow2 exist`. |
| `0017-gow2-clear-msaa.patch` | `src/xenia/gpu/d3d12/d3d12_command_processor.{h,cc}`, `src/xenia/gpu/render_target_cache.{h,cc}` | Cvar `gow2_clear_msaa_remap` (default false = Xenia unchanged; gow2 sets it to true in `main.cpp`): the XDK's D3D Clear draws a rectangle (RECTLIST, 3 vertices) with MSAA forced to 4x on the depth that a target with fewer samples already owns at the same base, and the ownership change cost two full EDRAM transfers (78% of the data transferred in a gameplay frame at 2x). `IssueDraw` recognizes the rectangle (no pixel shader or memexport, ALWAYS depth/stencil test, no window offset; viewport off with pixel positions, like the real Clear, or on and overflowing the surface at the left, top and right) and, if `RenderTargetCache::Gow2GetLowerMsaaDepthOwner` finds the base's owner (same tile pitch and format, fewer samples, not 2x because of the patch 0009 shift), rewrites for the duration of the draw `RB_SURFACE_INFO` (MSAA and pitch), viewport and scissor scaled by the sample ratio s (`Gow2ClearRemap`, RAII; viewport off: turned on with scale s; offset * s + (s - 1) * 0.5 with the D3D pixel center, because Xenia adds the half pixel after the scale) and excludes the draw from the upscaler jitter as at 4x. Exact equivalence where the 4 samples of a 4x pixel lie on the same side of the edges; without the patch, with the top edge at 0.25 px, Xenia lost part of the first row. `RenderTargetCache::Update` passes each draw's transfers to `gow2::hooks::OnEdramTransfers` (summary `gpu_edram_transfers`). Tests: `d3d12_msaa: 4x ... clear ...` (remapped without transfers with depth+stencil, stencil only, depth only, in screen coordinates on the whole surface and partial with OpenGL and D3D center; not remapped with a narrow viewport, D24S8 owner, 2x owner, color write), `stats: draws, skipped draws, resolves, EDRAM transfers ...`, `d3d12_backend: the Xenia cvars set by gow2 exist`. |
| `0018-gow2-resolve-fill-depth.patch` | `src/xenia/gpu/draw_util.{h,cc}` | Cvar `gow2_resolve_fill_depth` (default true = Xenia unchanged; gow2 sets it to false in `main.cpp`): with scaled resolution, `resolve_resolution_scale_fill_half_pixel_offset` copies the first host row and column of every resolve area from the second, to cover the half-pixel hole at the screen edges. GoW2 draws the scene in two bands (predicated tiling, rows 0-511 and 512-719) and also resolves its depth: on the second band, copying the first row left a dark line on the characters at row 512 (output 1024 at 1440p), at 2x and 3x with or without the upscaler. With the cvar false the fill stays for color resolves (clean screen edges) and is not done for depth ones. The decision lives in `draw_util::ResolveFillHalfPixelOffset`. Tests: `xenia_core: half pixel of scaled resolves ...`, `d3d12_backend: the Xenia cvars set by gow2 exist`. |
| `0019-gow2-host-grid-manual-filter.patch` | `src/xenia/gpu/dxbc_shader_translator_fetch.cc` | Manual bilinear filtering on the host grid at resolution scale > 1. A fetch constant the pixel shader also passes to `getWeights2D` is filtered by hand: point `tfetch` taps at instruction offsets (-0.5..1.5) blended with the weights (Gears of War 3's PCF on the 1120x688 shadow depth buffer, shaders 3C5E875D17B49A36, 857CECDEB64A657F, CA3F4A808B09D6F4). On a scaled texture Xenia put the weights on the guest grid (fraction of the guest texel) and, with `draw_resolution_scaled_texture_offsets=false` (gow2's default, for UE3's DOF downsample), the offsets too, but the point taps change host texel in the middle of a guest texel: the shadow mask had steps on the characters (banded, weaker self-shadows at 2x). For those fetch constants, and only when the texture is scaled (`textures_resolution_scaled`), the `tfetch` offsets are in host texels (the scaled-offset path, independent of the cvar) and `getWeights` takes the fraction of the host texel (coordinate * size * scale + offset): taps and weights on the same grid, as at 1x on a shadow map with 2x the texels. Only when every `tfetch` and `getWeights` on the constant reads the same coordinate register and XY swizzle (taps that differ only by instruction offsets): fetches without `getWeights2D` on the same constant, or with taps from another register (computed in ALU, which stay in guest texels), are unchanged. Register numbers are compared, not values (a `shortcut:` in the code). GoW2 not yet verified at 2x/3x. Tests: `xenia_core: manual bilinear filter on the host grid at resolution scale 2 (patch 0019)` (also getWeights on another constant and taps from another register). |
| `0020-gow2-submissions-at-scale.patch` | `src/xenia/gpu/d3d12/d3d12_command_processor.cc` | Cvar `d3d12_submit_every_draws_scaled` (default false = patch 0016 unchanged, only at scale 1; `main.cpp` sets it from `game_profile::Profile::submit_split_scaled`, true for GoW3): the submission every `d3d12_submit_every_draws` draws also at a draw resolution scale above 1. GoW3 at 3x with strict occlusion queries: the game waits for a query result with one submission per frame, so the GPU only started at the wait and the CPU waited for the whole frame (GPU profile of the prison cinematic: ~10 ms of GPU work, ~4.5 ms idle per frame); with the split the two overlap: 66 -> 88 fps in the cinematic, 79 -> 117 in Act 1 gameplay (RTX 4080). Per-frame ZPD statistics (reports, sample sum, zero reports) unchanged by the split. GoW2 keeps 1x only (blue armor at 3x, cause not found, see 0016). Tests: `d3d12_draw: ZPD report with one submission per draw at scale 2`, `d3d12_backend: the Xenia cvars set by gow2 exist`. |
| `0021-gow2-draw-observer.patch` | `src/xenia/gpu/d3d12/d3d12_command_processor.h`, `.cc`, `src/xenia/gpu/command_processor.h`, `.cc` | `SetDrawObserverHook`: a hook called on the command processor thread for every draw that reaches pipeline configuration, with the VS and PS translations used (hash and modification) and whether the draw was skipped (pipeline not ready with async compilation). Used only by the runtime's shader capture (`--shader-capture`, F11); nullptr = no hook. `SetZPDObserverHook`: each EVENT_WRITE_ZPD report when queued (command order) and its ZPass/ZFail delta when retired, for the capture's occlusion query lines. Tests: `d3d12_draw: draw observer reports the translations used`, `d3d12_draw: ZPD observer reports queued and retired reports`. |
| `0022-gow2-copy-depth-target.patch` | `src/xenia/gpu/d3d12/d3d12_render_target_cache.h`, `src/xenia/gpu/d3d12/d3d12_render_target_cache.cc`, `src/xenia/gpu/d3d12/d3d12_command_processor.h` | `Gow2SharedMemory` (accessor, for the shader capture's copies of the vertex data draws read) and `Gow2CopyDepthTarget`: copies the depth plane of the last Update's depth render target (host render target path) into a readback buffer in the current command list, leaving the render target in its prior state. Used by `--shader-capture` to read the half-resolution depth Gears of War 3's occlusion query boxes are tested against. |
| `0023-gow2-depth-pass-upper-left-texel.patch` | `src/xenia/gpu/dxbc_shader_translator_fetch.cc` | Depth-writing pixel shaders subtract the texture coordinate epsilon instead of adding it, so point-sampled fetches exactly on a texel boundary take the upper-left texel. Gears of War 3 builds its half-resolution occlusion query depth as the min of 2x2 point fetches at exact boundaries: with the lower-right texels the footprint is one full-resolution pixel off, the floors' query boxes fail against the floors themselves and distant floors flash. |
| `0024-gow2-query-box-jitter.patch` | `src/xenia/gpu/draw_util.h`, `src/xenia/gpu/d3d12/d3d12_command_processor.cc` | `draw_util::Gow2JitterScale`: the predicate of the upscaler jitter (patch 0013) as a function, extended to the occlusion query boxes (depth test without depth or color writes on the surface with half the scene pitch), which get half the jitter in their own pixels. The query depth is built from the jittered scene depth: without the jitter the boxes of Gears of War 3's distant floors fail against the floors themselves with DLSS and the floors flash. The scene draws (scale 1) keep selecting the LOD bias draws (patch 0014). |

## Shims

The shims live in `runtime/gpu/shims/` (GPU and common base) and
`runtime/apu/shims/` (APU) with the same include path as Xenia; the include
order is `runtime/apu/shims` -> `runtime/gpu/shims` ->
`thirdparty/xenia/src` -> `thirdparty/ffmpeg` (`cmake/XeniaGpu.cmake`,
`cmake/XeniaApu.cmake`). No path is both vendored and shimmed, and none is
shimmed in both folders (`tests/test_vendor_xenia.py`).
They only use the C++ standard library and the CRT.

| Shim | Replaces in Xenia | Interface provided |
|---|---|---|
| `xenia/base/logging.h`, `.cpp` | `src/xenia/base/logging.{h,cc}` | `XELOGE/W/I/D/CPU/APU/GPU/KERNEL/FS` (macros: at most 10 lines per call site) on top of `LogLine("[xenia] <c> ...")`; enums formatted as integers, non-`char` pointers as `const void*` (fmt 11 rejects `{:p}` on `uint8_t*`); `xe::FatalError` -> `[gpu] fatal:` line, hook `gow2::xenia::SetFatalErrorHook`, exit 5; for canary `logging::ShouldLog` (errors, warnings, info) and `LoggerBatch<level>`, includes `fmt/std.h` like canary |
| `xenia/base/cvar.h`, `.cpp` | `src/xenia/base/cvar.{h,cc}` | `DEFINE_*`/`DECLARE_*` (bool, int32, uint32, uint64, double, string, path, transient) with a registry by name; `gow2::xenia::SetCvar`, `gow2::xenia::CvarNames`; for canary `OVERRIDE_*` (assignment) and `UPDATE_from_*` (no effect: there is no configuration file) |
| `xenia/base/profiling.h`, `.cpp` | `src/xenia/base/profiling.{h,cc}` | empty `SCOPE_profile_*`/`COUNT_profile_*` macros, `Profiler` with empty methods |
| `xenia/base/threading.h`, `.cpp` | `src/xenia/base/threading{.h,.cc,_win.cc}` | `Fence` (several waiters, like Xenia), `Event`, `Thread::Create`, `Thread::GetCurrentThread`, `Suspend`/`Resume` (on std::thread only self-suspension), `Wait`, `Sleep`, `MaybeYield`, `SyncMemory`, `logical_processor_count` on std::thread/condition_variable; `gow2_SetCurrentThread` for the XThreads; for canary `Event::SetBoostPriority` = `Set` |
| `xenia/base/clock.h`, `.cpp` | `src/xenia/base/clock{.h,.cc,_win.cc}` | `Clock::QueryHostTickFrequency/TickCount/SystemTime/UptimeMillis` on std::chrono |
| `xenia/base/filesystem.h`, `.cpp` | `src/xenia/base/filesystem{.h,.cc,_win.cc}` | `path_to_utf8/16`, `to_path`, `filesystem::OpenFile/Seek/Tell/TruncateStdioFile/CreateParentFolder/CreateEmptyFile` |
| `xenia/base/string.h`, `.cpp` | `src/xenia/base/string.{h,cc}` | `to_utf8`, `to_utf16`, `xe_strcasecmp`, `xe_strncasecmp`, `xe_strdup` |
| `xenia/base/mutex.cpp` | `src/xenia/base/mutex.cc` | `global_critical_region::mutex()` for the vendored header `mutex.h`: in canary it is an `xe_global_mutex` (recursive SRWLOCK), whose `lock`/`unlock`/`try_lock` here forward to `physwatch::GlobalLock()` (runtime/physical_watch.h): a single global lock; `xe_fast_mutex` like canary |
| `xenia/base/platform_amd64.cpp` | `src/xenia/base/platform_amd64.cc` | `amd64::GetFeatureFlags()` with direct CPUID (canary uses xbyak, not vendored): only AVX2, BMI1, PrefetchW, Movdir64M, FastRepMovs, the flags read by the vendored code |
| `gow2_canary_std.h` | (none) | force-included (`/FI`, PUBLIC on `xenia_gpu_core`): standard library headers that in canary arrive from cxxopts/toml++/microprofile through `cvar.h` and `profiling.h` |
| `xenia/base/memory_host.cpp` | `src/xenia/base/memory_win.cc` | `memory::page_size()` = 4096, `allocation_granularity()` = 65536, `IsWritableExecutableMemorySupported()` = false, `AllocFixed`/`DeallocFixed` with `VirtualAlloc`/`VirtualFree` (`SharedMemory` page flags in canary), for the vendored header `memory.h` |
| `xenia/memory.h`, `.cpp` | `src/xenia/memory.{h,cc}` | `xe::Memory(guest_base)`: `TranslateVirtual<T>` = base + guest address (APU), with the +0x1000 offset of view 0xE (`gow2::HostOffset`, runtime/guest_address.h), `physical_membase()` = base + 0xA0000000, `TranslatePhysical<T>` (& 0x1FFFFFFF), invalidation callbacks registered in `physwatch` (runtime/physical_watch.h, under the global lock), `EnablePhysicalMemoryAccessCallbacks` -> `physwatch::Watch` (data providers logged once and ignored); for canary `GetPhysicalHeap()` (`VirtualHeap`: everything accessible and writable, gow2's physical memory is mapped in full) and include of `xenia/base/mutex.h` |
| `xenia/gpu/graphics_system.h`, `.cpp` | `src/xenia/gpu/graphics_system.{h,cc}` | `GraphicsSystem(Memory*, GraphicsProvider*, Presenter*)` without MMIO or thread: RegisterFile, owned CommandProcessor, `InitializeRingBuffer`/`EnableReadPointerWriteBack`, `DispatchInterruptCallback` -> `gpu::DispatchInterrupt`, `OnHostGpuLossFromAnyThread` (line `[gpu] device removed reason=0x%08X` from `gow2_DeviceRemovedReason`, then `xe::FatalError`: exit 5); bridge `gow2_PollWritePointer`/`gow2_MirrorRegister`/`gow2_ReadRegisterForWait` with the guest register region (COHER_STATUS_HOST cleared by MakeCoherent is also copied into the region). Builds with the runtime (`gow2_gpu_xenia`, C++23); for canary `GetScaledAspectRatio()` = 16:9 |
| `xenia/kernel/xthread.h`, `.cpp` | `src/xenia/kernel/xthread.{h,cc}`, `object_ref` of `xobject.h` | `XHostThread` on `kernel::GuestThread::CreateHost` (PCR, KTHREAD, guest stack), `Create`, `Wait`, `set_name`, `thread()` (Suspend/Resume of the guest thread), `IsInThread`; `object_ref` on shared_ptr. `.cpp` in `gow2_gpu_xenia`; for canary the fifth `guest_process` argument of `XHostThread` (ignored) |
| `xenia/kernel/kernel_state.h` | `src/xenia/kernel/kernel_state.h` | `KernelState::GetExecutableModule()` (only for trace names, never opened); for canary `memory()`, `GetIdleProcess()` = 0, `processor()->backend()->PopulatePseudoStacktrace` = false (log of `log_ringbuffer_kickoff_initiator_bts`), `emulator()` |
| `xenia/emulator.h`, `xenia/apu/audio_system.h` | `src/xenia/emulator.h`, `src/xenia/apu/audio_system.h` | `Emulator::audio_system()` = nullptr (window title of `D3D12CommandProcessor`; audio belongs to gow2) |
| `xenia/kernel/user_module.h` | `src/xenia/kernel/user_module.h` | `UserModule::title_id()` = 0 |
| `xenia/ui/presenter.h` | `src/xenia/ui/presenter.h` | abstract base class `Presenter`: `GuestOutputRefreshContext` (`SetIs8bpc`) and pure virtual `RefreshGuestOutput(w, h, ax, ay, refresher)` (non-virtual in Xenia), implemented by gow2's presenter |
| `xenia/ui/d3d12/d3d12_presenter.h` | `src/xenia/ui/d3d12/d3d12_presenter.{h,cc}` | abstract `D3D12Presenter` with `kGuestOutputFormat` (`R10G10B10A2_UNORM`), `kGuestOutputInternalState` (`PIXEL_SHADER_RESOURCE`) and `D3D12GuestOutputRefreshContext::resource_uav_capable()`, what `D3D12CommandProcessor::IssueSwap` uses |
| `xenia/gpu/d3d12/d3d12_graphics_system.h` | `src/xenia/gpu/d3d12/d3d12_graphics_system.{h,cc}` | `D3D12GraphicsSystem(Memory*, D3D12Provider*, Presenter*)` on the `GraphicsSystem` shim (provider not owned, presenter nullable), typed `provider()`, `gow2_DeviceRemovedReason` from the provider's device; the `D3D12CommandProcessor` (gow2 subclass in `runtime/gpu/d3d12_backend.cpp`) is created by the factory of `gpu::CreateXeniaBackend` |
| `third_party/DirectXShaderCompiler/projects/dxilconv/include/DxbcConverter.h` | submodule `third_party/DirectXShaderCompiler` | minimal declaration of `IDxbcConverter` and `CLSID_DxbcConverter` for `d3d12_api.h`; never instantiated (patch 0006) |
| `gow2_hooks.h` | (none: gow2 hooks for the patches) | `gow2::hooks::OnDraw`, `OnUnknownPacket`, implemented in `runtime/gpu/xenia_backend.cpp` |
| `xenia/apu/xma_decoder.h` (in `runtime/apu/shims/`) | `src/xenia/apu/xma_decoder.h` | without `XmaDecoder` (registers, worker and context allocation belong to gow2): the contexts include it without using it; it only brings `xenia/base/filesystem.h`, which the original reaches from `xenia/kernel/xthread.h` and `XmaContext::DumpRaw` uses |
| `xenia/xbox.h` (in `runtime/apu/shims/`) | `src/xenia/xbox.h` | only `xenia/base/assert.h` (`static_assert_size`) and `xenia/base/memory.h` (`copy_and_swap`), what `xma_context.h` uses |
| `third_party/FFmpeg/libavcodec/avcodec.h`, `third_party/FFmpeg/libavutil/channel_layout.h`, `third_party/FFmpeg/libavutil/error.h` (in `runtime/apu/shims/`) | submodule `third_party/FFmpeg` | forwarding to the same-named headers of the vendored FFmpeg subset (`thirdparty/ffmpeg`, `xenia_ffmpeg`) |
| `gow2_apu_hooks.h` (in `runtime/apu/shims/`) | (none: gow2 hooks for patch 0007) | `gow2::hooks::OnXmaFrameDecoded`, `OnXmaDecodeError`, implemented in `runtime/apu/xma_hooks.cpp` |
| `xenia/gpu/trace_writer.h` | `src/xenia/gpu/trace_writer.{h,cc}` | `TraceWriter` with `is_open()` = false and empty writes; `EventCommand::Type` of `trace_protocol.h`; with canary `EventCommand` comes from the vendored `trace_protocol.h` |
| `xenia/ui/graphics_provider.h` | `src/xenia/ui/graphics_provider.h` | `GraphicsProvider::GpuVendorID` and a base without `CreatePresenter`/`CreateImmediateDrawer` |
| `third_party/fmt/include/fmt/format.h` | submodule `third_party/fmt` | forwarding to `<fmt/format.h>` and `<fmt/xchar.h>` (XenonRecomp's `fmt::fmt`, fmt 11; the D3D12 backend formats wide strings) |
| `third_party/fmt/include/fmt/std.h`, `xchar.h` | submodule `third_party/fmt` | forwarding to `<fmt/std.h>` (canary's `logging.h` includes it) and `<fmt/xchar.h>` |
| `third_party/xxhash/xxhash.h` | submodule `third_party/xxhash` | forwarding to `<xxhash.h>` (XenonRecomp's `xxHash::xxhash`) |
| `third_party/DirectX-Headers/include/directx/{d3d12,d3d12sdklayers,dxgicommon,dxgiformat}.h` | submodule `third_party/DirectX-Headers` | forwarding to the Windows SDK headers |
| `third_party/DirectXShaderCompiler/include/dxc/dxcapi.h` | submodule `third_party/DirectXShaderCompiler` | forwarding to the Windows SDK's `<dxcapi.h>` |

## License of third_party/dxbc

`third_party/dxbc/DXBCChecksum.{h,cpp}` compute the checksum of DXBC
containers. Copyright 2008-2016 Advanced Micro Devices, Inc. (AMD Developer
Tools Team); Xenia modified them to drop the Windows.h dependency. The code
is a "free for all" MD5 implementation (as the comment in `DXBCChecksum.cpp`
calls it), with the style modified by Tony Ray (2001) and Dominik Reichl
(2002), derived from the "RSA Data Security, Inc. MD5 Message Digest
Algorithm" (Copyright (C) 1990, RSA Data Security, Inc.): the RSA license in
the header of `DXBCChecksum.h` asks to identify derived works this way and to
keep its notices, which remain intact in the files.
