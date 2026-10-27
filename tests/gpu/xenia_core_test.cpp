// Vendored Xenia GPU core (thirdparty/xenia, xenia_gpu_core library)
// with the runtime/gpu/shims shims: registers, swap, tiling, analysis and
// DXBC translation of the microcode of tests/gpu/ucode_builder.h, cvars and log.
// No GPU: the DXBC translator only produces bytes.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "gpu/ucode_builder.h"
#include "guest_memory.h"
#include "log.h"
#include "physical_watch.h"
#include "temp_dir.h"
#include "third_party/dxbc/DXBCChecksum.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/threading.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/dxbc_shader_translator.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"

// The cvar is defined by the vendored D3D12 backend
// (d3d12_render_target_cache.cc in xenia_gpu_d3d12): this reference makes
// its object get linked, which registers it.
DECLARE_string(render_target_path_d3d12);
DECLARE_bool(dxbc_switch);
DECLARE_string(readback_resolve);
DECLARE_string(occlusion_query);

// Xenia's code (C++20) uses the STL's std::endian: MSVC values.
static_assert(int(std::endian::little) == 0 && int(std::endian::big) == 1);
static_assert(std::endian::native == std::endian::little);
DECLARE_int32(anisotropic_override);

namespace {

using namespace xe::gpu;
namespace ub = gow2::ucode_builder;

std::unique_ptr<DxbcShader> MakeShader(xenos::ShaderType type, const std::vector<uint32_t>& ucode) {
  auto shader = std::make_unique<DxbcShader>(type, 0x1234, ucode.data(), ucode.size(),
                                             std::endian::native);
  xe::StringBuffer disasm;
  shader->AnalyzeUcode(disasm);
  return shader;
}

std::vector<uint8_t> Translate(DxbcShader& shader) {
  DxbcShaderTranslator translator(xe::ui::GraphicsProvider::GpuVendorID::kNvidia,
                                  /*bindless_resources_used=*/false,
                                  /*edram_rov_used=*/false);
  const uint64_t modification =
      shader.type() == xenos::ShaderType::kVertex
          ? translator.GetDefaultVertexShaderModification(shader.GetDynamicAddressableRegisterCount(0))
          : translator.GetDefaultPixelShaderModification(shader.GetDynamicAddressableRegisterCount(0));
  Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  REQUIRE(translation != nullptr);
  REQUIRE(translator.TranslateAnalyzedShader(*translation));
  REQUIRE(translation->is_valid());
  return translation->translated_binary();
}

void RequireDxbcContainer(std::vector<uint8_t> dxbc) {
  REQUIRE(dxbc.size() > 32);
  CHECK(std::memcmp(dxbc.data(), "DXBC", 4) == 0);
  uint32_t stored[4];
  std::memcpy(stored, dxbc.data() + 4, sizeof(stored));
  unsigned int computed[4] = {};
  CalculateDXBCChecksum(dxbc.data(), static_cast<unsigned int>(dxbc.size()), computed);
  CHECK(std::memcmp(stored, computed, sizeof(stored)) == 0);
}

// Flag of a cbuffer constant in the RDEF of the DXBC container (nullopt if the name is not there).
std::optional<uint32_t> RdefVariableFlags(const std::vector<uint8_t>& dxbc_bytes, std::string_view name) {
  dxbc::ContainerHeader header;
  std::memcpy(&header, dxbc_bytes.data(), sizeof(header));
  const uint32_t* offsets = reinterpret_cast<const uint32_t*>(dxbc_bytes.data() + sizeof(header));
  for (uint32_t b = 0; b < header.blob_count; ++b) {
    dxbc::BlobHeader blob;
    std::memcpy(&blob, dxbc_bytes.data() + offsets[b], sizeof(blob));
    if (blob.fourcc != dxbc::BlobHeader::FourCC::kResourceDefinition) continue;
    const uint8_t* rdef = dxbc_bytes.data() + offsets[b] + sizeof(blob);
    dxbc::RdefHeader rh;
    std::memcpy(&rh, rdef, sizeof(rh));
    for (uint32_t c = 0; c < rh.cbuffer_count; ++c) {
      dxbc::RdefCbuffer cb;
      std::memcpy(&cb, rdef + rh.cbuffers_ptr + c * sizeof(cb), sizeof(cb));
      for (uint32_t v = 0; v < cb.variable_count; ++v) {
        dxbc::RdefVariable var;
        std::memcpy(&var, rdef + cb.variables_ptr + v * sizeof(var), sizeof(var));
        if (name == reinterpret_cast<const char*>(rdef + var.name_ptr)) return var.flags;
      }
    }
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("xenia_core: register table", "[xenia_core]") {
  const RegisterInfo* info = RegisterFile::GetRegisterInfo(0x01C5);
  REQUIRE(info != nullptr);
  CHECK(std::string(info->name) == "CP_RB_WPTR");
}

TEST_CASE("xenia_core: GpuSwap 8in32", "[xenia_core]") {
  CHECK(xenos::GpuSwap(uint32_t(0x11223344), xenos::Endian::k8in32) == 0x44332211u);
}

TEST_CASE("xenia_core: 2D tiled offset", "[xenia_core]") {
  CHECK(texture_address::Tiled2D(0, 0, 64, 2) == 0);
  // (x, y) = (32, 0), 32 bpp (log2 = 2 bytes per block), pitch 64 blocks,
  // with the formula of texture_address.h:
  //   outer_blocks = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6 = (0 + 1) << 6 = 64
  //   inner_blocks = (((y >> 1) & 7) << 3) | (x & 7)          = 0
  //   outer_inner_bytes = (64 | 0) << 2                       = 256
  //   bank = (y >> 4) & 1 = 0; pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1) = 4 & 3 = 0
  //   TiledCombine: (256 & 15) | ((256 >> 4) & 1) << 5 | ((256 >> 5) & 7) << 8 | (256 >> 8) << 12
  //               = 0 | 0 | 0 | 1 << 12                       = 4096
  const int32_t expected = 4096;
  CHECK(texture_address::Tiled2D(32, 0, 64, 2) == expected);
}

TEST_CASE("xenia_core: half pixel of scaled resolves, depth excluded with gow2_resolve_fill_depth (patch 0018)",
          "[xenia_core]") {
  // GoW2 resolves the scene in two bands (rows 0-511 and 512-719): filling the first host row of the
  // second band's depth resolve leaves a dark line on the characters at row 512.
  auto regs = std::make_unique<RegisterFile>();
  (*regs)[XE_GPU_REG_PA_SU_VTX_CNTL] = 0;  // pix_center = kD3DZero
  CHECK(!draw_util::ResolveFillHalfPixelOffset(*regs, 1, 1, false));
  CHECK(draw_util::ResolveFillHalfPixelOffset(*regs, 2, 2, false));
  REQUIRE(gow2::xenia::SetCvar("gow2_resolve_fill_depth", "true"));  // Xenia's default
  CHECK(draw_util::ResolveFillHalfPixelOffset(*regs, 2, 2, true));
  REQUIRE(gow2::xenia::SetCvar("gow2_resolve_fill_depth", "false"));  // as gow2 sets it
  CHECK(!draw_util::ResolveFillHalfPixelOffset(*regs, 2, 2, true));
  CHECK(draw_util::ResolveFillHalfPixelOffset(*regs, 2, 2, false));
  REQUIRE(gow2::xenia::SetCvar("gow2_resolve_fill_depth", "true"));
}

TEST_CASE("xenia_core: ucode_builder respects the ucode.h structs", "[xenia_core]") {
  const std::vector<uint32_t> vs = ub::VsPositionFromStream();
  REQUIRE(vs.size() == 9);
  ucode::ControlFlowInstruction cf[2];
  ucode::UnpackControlFlowInstructions(vs.data(), cf);
  CHECK(cf[0].opcode() == ucode::ControlFlowOpcode::kAlloc);
  CHECK(cf[0].alloc.alloc_type() == ucode::AllocType::kVsPosition);
  CHECK(cf[1].opcode() == ucode::ControlFlowOpcode::kExecEnd);
  CHECK(cf[1].exec.address() == 1);
  CHECK(cf[1].exec.count() == 2);
  ucode::VertexFetchInstruction vf;
  std::memcpy(&vf, vs.data() + 3, sizeof(vf));
  CHECK(vf.opcode() == ucode::FetchOpcode::kVertexFetch);
  CHECK(vf.fetch_constant_index() == 0);
  CHECK(vf.stride() == 3);
  CHECK(vf.data_format() == xenos::VertexFormat::k_32_32_32_FLOAT);
  CHECK(!vf.is_mini_fetch());
  ucode::AluInstruction alu;
  std::memcpy(&alu, vs.data() + 6, sizeof(alu));
  CHECK(alu.is_export());
  CHECK(alu.vector_dest() == uint32_t(ucode::ExportRegister::kVSPosition));
  CHECK(alu.vector_opcode() == ucode::AluVectorOpcode::kMax);
  CHECK(alu.src_is_temp(1));

  const std::vector<uint32_t> ps = ub::PsTexture2D();
  ucode::TextureFetchInstruction tf;
  std::memcpy(&tf, ps.data() + 3, sizeof(tf));
  CHECK(tf.opcode() == ucode::FetchOpcode::kTextureFetch);
  CHECK(tf.dimension() == xenos::FetchOpDimension::k2D);
  CHECK(tf.fetch_constant_index() == 0);
  CHECK(!tf.has_mag_filter());
}

TEST_CASE("xenia_core: VS position from vf0 translated to DXBC", "[xenia_core]") {
  auto shader = MakeShader(xenos::ShaderType::kVertex, ub::VsPositionFromStream());
  REQUIRE(shader->is_ucode_analyzed());
  REQUIRE(shader->vertex_bindings().size() == 1);
  const Shader::VertexBinding& binding = shader->vertex_bindings()[0];
  CHECK(binding.fetch_constant == 0);
  CHECK(binding.stride_words == 3);
  REQUIRE(binding.attributes.size() == 1);
  CHECK(binding.attributes[0].fetch_instr.attributes.data_format ==
        xenos::VertexFormat::k_32_32_32_FLOAT);
  CHECK(shader->texture_bindings().empty());
  RequireDxbcContainer(Translate(*shader));
}

TEST_CASE("xenia_core: constant color PS", "[xenia_core]") {
  auto shader = MakeShader(xenos::ShaderType::kPixel, ub::PsConstantColor());
  REQUIRE(shader->is_ucode_analyzed());
  CHECK(shader->writes_color_target(0));
  CHECK(!shader->writes_color_target(1));
  CHECK(shader->texture_bindings().empty());
  RequireDxbcContainer(Translate(*shader));
}

TEST_CASE("xenia_core: VS position+texcoord and PS tfetch2D", "[xenia_core]") {
  auto vs = MakeShader(xenos::ShaderType::kVertex, ub::VsPositionTexcoord());
  REQUIRE(vs->is_ucode_analyzed());
  REQUIRE(vs->vertex_bindings().size() == 1);
  CHECK(vs->vertex_bindings()[0].stride_words == 5);
  CHECK(vs->vertex_bindings()[0].attributes.size() == 2);
  RequireDxbcContainer(Translate(*vs));

  auto ps = MakeShader(xenos::ShaderType::kPixel, ub::PsTexture2D());
  REQUIRE(ps->is_ucode_analyzed());
  CHECK(ps->writes_color_target(0));
  REQUIRE(ps->texture_bindings().size() == 1);
  CHECK(ps->texture_bindings()[0].fetch_constant == 0);
  RequireDxbcContainer(Translate(*ps));
}

TEST_CASE("xenia_core: xe::Memory shim", "[xenia_core]") {
  // Pointer arithmetic only: no memory access.
  uint8_t* const fake_base = reinterpret_cast<uint8_t*>(uintptr_t(0x100000000ull));
  xe::Memory memory(fake_base);
  CHECK(memory.physical_membase() == fake_base + 0xA0000000u);
  // TranslateVirtual: base + guest address, with the 4 KB offset of the 0xE view.
  CHECK(memory.TranslateVirtual(0xA0000010) == fake_base + 0xA0000010u);
  CHECK(memory.TranslateVirtual(0xE0000010) == fake_base + 0xE0001010u);
  // The 0xA/0xC/0xE mirrors and the pure physical address give the same host.
  CHECK(memory.TranslatePhysical(0x00001000) == fake_base + 0xA0001000u);
  CHECK(memory.TranslatePhysical(0xA0001000) == fake_base + 0xA0001000u);
  CHECK(memory.TranslatePhysical(0xC0001000) == fake_base + 0xA0001000u);
  CHECK(memory.TranslatePhysical<const uint32_t*>(0xE0001000) ==
        reinterpret_cast<const uint32_t*>(fake_base + 0xA0001000u));
  auto callback = [](void*, uint32_t, uint32_t, bool) { return std::pair<uint32_t, uint32_t>(0, 0); };
  void* a = memory.RegisterPhysicalMemoryInvalidationCallback(callback, nullptr);
  void* b = memory.RegisterPhysicalMemoryInvalidationCallback(callback, &memory);
  CHECK(a != b);
  CHECK(memory.gow2_invalidation_callback_count() == 2);
  memory.EnablePhysicalMemoryAccessCallbacks(0x1000, 0x1000, true, false);
  memory.UnregisterPhysicalMemoryInvalidationCallback(a);
  CHECK(memory.gow2_invalidation_callback_count() == 1);
  memory.UnregisterPhysicalMemoryInvalidationCallback(b);
  CHECK(memory.gow2_invalidation_callback_count() == 0);
}

TEST_CASE("xenia_core: global_critical_region is physwatch's lock", "[xenia_core]") {
  xe::global_critical_region region;
  auto lock = region.Acquire();
  // another thread does not take physwatch's lock: it is the same
  bool other_thread_locked = true;
  std::thread([&] {
    other_thread_locked = physwatch::GlobalLock().try_lock();
    if (other_thread_locked) {
      physwatch::GlobalLock().unlock();
    }
  }).join();
  CHECK_FALSE(other_thread_locked);
  // recursive and shared: the same thread retakes it from physwatch
  CHECK(physwatch::GlobalLock().try_lock());
  physwatch::GlobalLock().unlock();
}

namespace {
struct XeniaWatchCalls {
  std::atomic<int> calls{0};
  std::atomic<uint32_t> phys{0}, len{0};
  std::atomic<bool> inside{false}, done{false};
  bool slow = false;
  static std::pair<uint32_t, uint32_t> Callback(void* ctx, uint32_t phys, uint32_t len, bool) {
    auto* self = static_cast<XeniaWatchCalls*>(ctx);
    // like Xenia's callbacks: the global lock is already taken (recursive)
    auto lock = xe::global_critical_region::AcquireDirect();
    self->calls.fetch_add(1);
    self->phys = phys;
    self->len = len;
    self->inside = true;
    if (self->slow) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    self->done = true;
    return {phys, len};
  }
};
}  // namespace

TEST_CASE("xenia_core: xe::Memory registers in physwatch and observes with EnablePhysicalMemoryAccessCallbacks",
          "[xenia_core]") {
  GuestMemory mem;
  REQUIRE(mem.Init());
  physwatch::Init(mem);
  xe::Memory memory(mem.base);
  XeniaWatchCalls calls;
  void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(&XeniaWatchCalls::Callback, &calls);
  CHECK(memory.gow2_invalidation_callback_count() == 1);

  gow2_test::TempDir dir("xenia_core_physwatch");
  const auto log_path = dir.path / "log.txt";
  LogInit(log_path);
  // physical address (also in mirror form: & 0x1FFFFFFF)
  memory.EnablePhysicalMemoryAccessCallbacks(0xC0123000, 0x1000, true, false);
  CHECK(physwatch::IsWatched(0x123000));
  memory.TranslatePhysical<volatile uint32_t*>(0x123010)[0] = 0xABCD;
  CHECK(calls.calls == 1);
  CHECK(calls.phys == 0x123000);
  CHECK(calls.len == 0x1000);
  CHECK(mem.base[0xE0123010] == 0xCD);
  // data provider: logged once and ignored; the invalidation stays
  memory.EnablePhysicalMemoryAccessCallbacks(0x124000, 0x1000, true, true);
  memory.EnablePhysicalMemoryAccessCallbacks(0x125000, 0x1000, false, true);
  CHECK(physwatch::IsWatched(0x124000));
  CHECK_FALSE(physwatch::IsWatched(0x125000));
  LogInit({});
  std::ifstream in(log_path);
  std::string line;
  int provider_lines = 0;
  while (std::getline(in, line)) {
    if (line.find("data providers not supported") != std::string::npos) {
      ++provider_lines;
    }
  }
  in.close();
  CHECK(provider_lines <= 1);  // once per process (other tests may have already logged it)

  // Unregister during a callback in progress on another thread: waits for it
  // to finish (global lock), then the callback is no longer called.
  calls.slow = true;
  calls.inside = false;
  calls.done = false;
  std::thread writer([&] { mem.base[0xA0124000] = 1; });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!calls.inside && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  if (!calls.inside) {
    FAIL_CHECK("the callback did not start within 2 s");
    std::_Exit(1);  // the writing thread might be blocked
  }
  memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
  CHECK(calls.done);
  writer.join();
  CHECK(calls.calls == 2);
  CHECK(memory.gow2_invalidation_callback_count() == 0);
  memory.EnablePhysicalMemoryAccessCallbacks(0x126000, 0x1000, true, false);
  mem.base[0xA0126000] = 1;
  CHECK(calls.calls == 2);

  // the destructor removes the remaining callbacks
  {
    xe::Memory other(mem.base);
    other.RegisterPhysicalMemoryInvalidationCallback(&XeniaWatchCalls::Callback, &calls);
  }
  memory.EnablePhysicalMemoryAccessCallbacks(0x127000, 0x1000, true, false);
  mem.base[0xC0127000] = 1;
  CHECK(calls.calls == 2);
}

TEST_CASE("xenia_core: SetCvar", "[xenia_core]") {
  CHECK(gow2::xenia::SetCvar("render_target_path_d3d12", "rov"));
  CHECK(cvars::render_target_path_d3d12 == "rov");
  CHECK(!gow2::xenia::SetCvar("does_not_exist", "1"));
  // Bool: invalid value rejected, previous value intact.
  const bool before = cvars::dxbc_switch;
  CHECK(!gow2::xenia::SetCvar("dxbc_switch", "maybe"));
  CHECK(cvars::dxbc_switch == before);
  CHECK(gow2::xenia::SetCvar("dxbc_switch", before ? "false" : "true"));
  CHECK(cvars::dxbc_switch == !before);
  // Choice strings: Xenia would treat an unknown value as the default, here it is rejected.
  const std::string readback = cvars::readback_resolve;
  CHECK(!gow2::xenia::SetCvar("readback_resolve", "true"));
  CHECK(cvars::readback_resolve == readback);
  CHECK(!gow2::xenia::SetCvar("occlusion_query", "quick"));
  CHECK(!gow2::xenia::SetCvar("readback_resolve", ""));
  CHECK(gow2::xenia::SetCvar("occlusion_query", "fast-alt"));
  CHECK(gow2::xenia::SetCvar("occlusion_query", "fast"));
  CHECK(gow2::xenia::SetCvar("readback_resolve", "full"));
  CHECK(gow2::xenia::SetCvar("readback_resolve", readback));
  // Integers: the cvars of the linked files are registered (gpu_flags.cc is linked
  // because shader.cc uses cvars::dump_shaders).
  const auto names = gow2::xenia::CvarNames();
  CHECK(std::is_sorted(names.begin(), names.end()));
  CHECK(std::find(names.begin(), names.end(), "render_target_path_d3d12") != names.end());
  CHECK(std::find(names.begin(), names.end(), "dxbc_switch") != names.end());
  CHECK(!gow2::xenia::SetCvar("anisotropic_override", "12x"));
  CHECK(cvars::anisotropic_override == -1);
  CHECK(gow2::xenia::SetCvar("anisotropic_override", "4"));
  CHECK(cvars::anisotropic_override == 4);
  CHECK(gow2::xenia::SetCvar("anisotropic_override", "-1"));
  // Unsigned: "-1" rejected.
  uint32_t u = 7;
  CHECK(!cvar::ParseValue("-1", u));
  CHECK(cvar::ParseValue("4294967295", u));
  CHECK(u == 4294967295u);
  CHECK(!cvar::ParseValue("4294967296", u));
}

namespace {
void LogFromOneCallSite() { XELOGE("test error {}", 42); }
}  // namespace

TEST_CASE("xenia_core: XELOG at most 10 lines per call site", "[xenia_core]") {
  gow2_test::TempDir dir("xenia_core_log");
  const auto log_path = dir.path / "log.txt";
  LogInit(log_path);
  for (int i = 0; i < 20; ++i) {
    LogFromOneCallSite();
  }
  XELOGW("another call site");
  LogInit({});
  std::ifstream in(log_path);
  std::string line;
  int same_site = 0, other = 0;
  while (std::getline(in, line)) {
    if (line.rfind("[xenia]", 0) != 0) {
      continue;
    }
    if (line.find("test error 42") != std::string::npos) {
      ++same_site;
    } else if (line.find("another call site") != std::string::npos) {
      ++other;
    }
  }
  CHECK(same_site == 10);
  CHECK(other == 1);
}

TEST_CASE("xenia_core: threading shim", "[xenia_core]") {
  using namespace std::chrono_literals;
  namespace th = xe::threading;
  auto auto_event = th::Event::CreateAutoResetEvent(false);
  CHECK(th::Wait(auto_event.get(), false, 0ms) == th::WaitResult::kTimeout);
  auto_event->Set();
  CHECK(th::Wait(auto_event.get(), false, 0ms) == th::WaitResult::kSuccess);
  // Automatic: the first wait consumed it.
  CHECK(th::Wait(auto_event.get(), false, 0ms) == th::WaitResult::kTimeout);

  auto manual_event = th::Event::CreateManualResetEvent(true);
  CHECK(th::Wait(manual_event.get(), false, 0ms) == th::WaitResult::kSuccess);
  CHECK(th::Wait(manual_event.get(), false, 0ms) == th::WaitResult::kSuccess);
  manual_event->Reset();
  CHECK(th::Wait(manual_event.get(), false, 0ms) == th::WaitResult::kTimeout);

  std::atomic<int> ran{0};
  auto go = th::Event::CreateManualResetEvent(false);
  auto thread = th::Thread::Create({}, [&] {
    th::Wait(go.get(), false);
    ran = 1;
  });
  CHECK(th::Wait(thread.get(), false, 10ms) == th::WaitResult::kTimeout);
  go->Set();
  CHECK(th::Wait(thread.get(), false) == th::WaitResult::kSuccess);
  CHECK(ran == 1);
  CHECK(th::logical_processor_count() >= 1);
}

TEST_CASE("xenia_core: string shim", "[xenia_core]") {
  // a, U+00E8, U+20AC, U+1F600 (surrogate pair in UTF-16).
  const std::u16string wide = {0x0061, 0x00E8, 0x20AC, 0xD83D, 0xDE00};
  const std::string utf8 = "\x61\xC3\xA8\xE2\x82\xAC\xF0\x9F\x98\x80";
  CHECK(xe::to_utf8(wide) == utf8);
  CHECK(xe::to_utf16(utf8) == wide);
  // Invalid bytes and isolated surrogates -> U+FFFD.
  CHECK(xe::to_utf16("x\xFFy") == std::u16string({0x0078, 0xFFFD, 0x0079}));
  CHECK(xe::to_utf8(std::u16string({0xD800, 0x0041})) == "\xEF\xBF\xBD\x41");
  CHECK(xe::xe_strcasecmp("Rtv", "rTV") == 0);
  CHECK(xe::xe_strcasecmp("a", "b") < 0);
}

TEST_CASE("xenia_core: Fence unblocks all waiters like Xenia", "[xenia_core]") {
  using namespace std::chrono_literals;
  namespace th = xe::threading;
  th::Fence fence;
  std::atomic<int> woke{0};
  auto w1 = th::Thread::Create({}, [&] {
    fence.Wait();
    ++woke;
  });
  auto w2 = th::Thread::Create({}, [&] {
    fence.Wait();
    ++woke;
  });
  std::this_thread::sleep_for(20ms);  // both inside Wait()
  fence.Signal();
  CHECK(th::Wait(w1.get(), false, 1000ms) == th::WaitResult::kSuccess);
  CHECK(th::Wait(w2.get(), false, 1000ms) == th::WaitResult::kSuccess);
  CHECK(woke == 2);
  // signal before the wait: it is not lost, and the last waiter clears it
  fence.Signal();
  fence.Wait();
  auto w3 = th::Thread::Create({}, [&] { fence.Wait(); });
  CHECK(th::Wait(w3.get(), false, 30ms) == th::WaitResult::kTimeout);
  fence.Signal();
  CHECK(th::Wait(w3.get(), false, 1000ms) == th::WaitResult::kSuccess);
}

TEST_CASE("xenia_core: Thread::GetCurrentThread and self-suspension", "[xenia_core]") {
  using namespace std::chrono_literals;
  namespace th = xe::threading;
  CHECK(th::Thread::GetCurrentThread() == nullptr);  // thread not created by the shim
  std::atomic<th::Thread*> self{nullptr};
  std::atomic<int> step{0};
  auto thread = th::Thread::Create({}, [&] {
    self = th::Thread::GetCurrentThread();
    step = 1;
    uint32_t previous = 99;
    th::Thread::GetCurrentThread()->Suspend(&previous);  // stops here
    step = previous == 0 ? 2 : -1;
  });
  while (step.load() == 0) std::this_thread::sleep_for(1ms);
  CHECK(self.load() == thread.get());
  CHECK_FALSE(thread->Suspend());  // from outside: not supported on std::thread
  std::this_thread::sleep_for(20ms);
  CHECK(step == 1);  // suspended
  // Resume until the thread finishes (a Resume that arrives before the
  // suspension has no effect, like ResumeThread): one sees count 1.
  bool resumed_from_one = false;
  for (int i = 0; i < 100 && th::Wait(thread.get(), false, 0ms) == th::WaitResult::kTimeout; ++i) {
    uint32_t previous = 0;
    CHECK(thread->Resume(&previous));
    resumed_from_one |= previous == 1;
    std::this_thread::sleep_for(10ms);
  }
  CHECK(th::Wait(thread.get(), false, 1000ms) == th::WaitResult::kSuccess);
  CHECK(resumed_from_one);
  CHECK(step == 2);
}

// Patch 0014: the upscaler's LOD bias lives in the slot that was padding (layout unchanged) and fetches with
// computed LOD read it.
TEST_CASE("xenia_core: upscaler mip bias in the system constants", "[xenia_core]") {
  using SC = DxbcShaderTranslator::SystemConstants;
  CHECK(offsetof(SC, gow2_mip_bias) == offsetof(SC, param_gen_window_offset) + 2 * sizeof(float));
  CHECK(sizeof(SC) == offsetof(SC, param_gen_window_offset) + 4 * sizeof(uint32_t));
  CHECK(sizeof(SC) % 16 == 0);

  auto ps = MakeShader(xenos::ShaderType::kPixel, ub::PsTexture2D());
  REQUIRE(ps->is_ucode_analyzed());
  const auto bin = Translate(*ps);
  RequireDxbcContainer(bin);
  const auto flags = RdefVariableFlags(bin, "xe_gow2_mip_bias");
  REQUIRE(flags);
  CHECK((*flags & dxbc::kRdefVariableFlagUsed) != 0);
  // A shader without texture fetches does not use it.
  const auto plain = Translate(*MakeShader(xenos::ShaderType::kPixel, ub::PsConstantColor()));
  const auto plain_flags = RdefVariableFlags(plain, "xe_gow2_mip_bias");
  CHECK((!plain_flags || (*plain_flags & dxbc::kRdefVariableFlagUsed) == 0));
  // Neither does a VS with LOD computed from register gradients: the bias follows the render pixels, in PS only.
  auto vs = MakeShader(xenos::ShaderType::kVertex, ub::VsTexture2DRegisterGradients());
  REQUIRE(vs->is_ucode_analyzed());
  REQUIRE(vs->texture_bindings().size() == 1);
  const auto vs_bin = Translate(*vs);
  RequireDxbcContainer(vs_bin);
  const auto vs_flags = RdefVariableFlags(vs_bin, "xe_gow2_mip_bias");
  CHECK((!vs_flags || (*vs_flags & dxbc::kRdefVariableFlagUsed) == 0));
}
