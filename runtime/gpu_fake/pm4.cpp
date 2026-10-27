// Mini PM4 interpreter (see pm4.h).
//
// Packet semantics from Xenia (https://github.com/xenia-project/xenia @
// 95a5c3ee, BSD), src/xenia/gpu/command_processor.cc: ExecutePacket,
// ExecutePacketType0..3 e ExecutePacketType3_{ME_INIT, NOP, INTERRUPT,
// XE_SWAP, INDIRECT_BUFFER, WAIT_REG_MEM, REG_RMW, REG_TO_MEM, MEM_WRITE,
// COND_WRITE, EVENT_WRITE, EVENT_WRITE_SHD, EVENT_WRITE_EXT, EVENT_WRITE_ZPD},
// SET_BIN_* e MakeCoherent; xenos.h: GpuSwap, CpuToGpu.
//
// Deviations: type 3 packets that are not executed (draws, states, shaders,
// ...) are skipped by length and counted instead of being rendered; a packet
// with fewer DWORDs than the minimum its handler reads is skipped as unknown
// (Xenia would read past it); indirect buffers nested beyond
// kMaxIndirectDepth are skipped; WAIT_REG_MEM sleeps in steps of at most
// 1 ms while checking running() (Xenia sleeps wait/0x100 ms in one go).
#include "gpu_fake/pm4.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "kernel/stats.h"
#include "log.h"
#include "platform/platform.h"

namespace gpu_fake {
namespace {

constexpr int kMaxIndirectDepth = 8;

// Xenia xenos.h GpuSwap (Endian: 0 none, 1 8in16, 2 8in32, 3 16in32).
uint32_t GpuSwap(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    case 2: return std::byteswap(v);
    case 3: return (v >> 16) | (v << 16);
    default: return v;
  }
}

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%02X", v);
  return buf;
}

// Logs once per opcode in the whole process.
std::atomic<uint64_t> g_logged_lo{0}, g_logged_hi{0};
bool FirstTime(uint32_t opcode) {
  std::atomic<uint64_t>& bits = opcode < 64 ? g_logged_lo : g_logged_hi;
  const uint64_t bit = 1ull << (opcode & 63);
  return (bits.fetch_or(bit) & bit) == 0;
}

// Circular reader of big-endian DWORDs (Xenia RingBuffer + ReadAndSwap).
struct Reader {
  uint8_t* base;
  uint32_t guest;  // guest address of the first DWORD of the buffer
  uint32_t size;   // DWORDs in the buffer
  uint32_t pos;    // index of the next DWORD
  uint32_t left;   // DWORDs still to read

  uint32_t Read() {
    uint32_t v;
    std::memcpy(&v, base + guest + pos * 4, 4);
    pos = (pos + 1) % size;
    --left;
    return std::byteswap(v);
  }
  void Skip(uint32_t n) {
    pos = uint32_t((uint64_t(pos) + n) % size);
    left -= n;
  }
};

struct Interp {
  uint8_t* base;
  const Pm4Effects& fx;
  Pm4State& st;

  uint8_t* Phys(uint32_t addr) const { return base + PhysicalToGuest(addr); }
  uint32_t LoadPhys(uint32_t addr) const {
    uint32_t v;
    std::memcpy(&v, Phys(addr), 4);
    return v;
  }
  void StorePhys(uint32_t addr, uint32_t v) const { std::memcpy(Phys(addr), &v, 4); }

  void WriteReg(uint32_t reg, uint32_t value) const {
    if (fx.write_reg) fx.write_reg(reg, value);
  }
  uint32_t ReadReg(uint32_t reg) const { return fx.read_reg ? fx.read_reg(reg) : 0; }
  bool Running() const { return !fx.running || fx.running(); }

  // Sleeps `duration` in steps of at most 1 ms; stops as soon as running() is false.
  void SleepInterruptible(std::chrono::microseconds duration) const {
    const auto end = std::chrono::steady_clock::now() + duration;
    for (auto now = std::chrono::steady_clock::now(); now < end && Running(); now = std::chrono::steady_clock::now()) {
      const auto left = std::chrono::duration_cast<std::chrono::microseconds>(end - now).count();
      plat::SleepPrecise(uint32_t(std::min<int64_t>(left, 1000)));
    }
  }

  // Xenia MakeCoherent: COHER_STATUS_HOST with status (bit 31 set) -> 0.
  void MakeCoherent() const {
    if (!(ReadReg(kRegCoherStatusHost) & 0x80000000u)) return;
    if (fx.store_reg)
      fx.store_reg(kRegCoherStatusHost, 0);
    else
      WriteReg(kRegCoherStatusHost, 0);
  }

  static bool Compare(uint32_t function, uint32_t value, uint32_t ref) {
    switch (function & 7) {
      case 1: return value < ref;
      case 2: return value <= ref;
      case 3: return value == ref;
      case 4: return value != ref;
      case 5: return value >= ref;
      case 6: return value > ref;
      case 7: return true;
      default: return false;  // 0: never
    }
  }

  // Executes all the reader's packets; false if a packet is truncated.
  bool Execute(Reader& r, int depth) {
    while (r.left) {
      if (!Packet(r, depth)) return false;
    }
    return true;
  }

  void LogTruncated(uint32_t packet, uint32_t count, uint32_t left) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 10)
      LogLine("[gpu] packet " + Hex(packet) + " truncated: " + std::to_string(count) + " DWORDs, " +
              std::to_string(left) + " left");
  }

  bool Packet(Reader& r, int depth) {
    const uint32_t packet = r.Read();
    ++st.counters.packets;
    if (packet == 0) return true;  // Xenia: a zero DWORD = one-DWORD no-op
    switch (packet >> 30) {
      case 0: {
        const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
        if (r.left < count) {
          LogTruncated(packet, count, r.left);
          return false;
        }
        const uint32_t index = packet & 0x7FFF;
        const bool one_reg = (packet >> 15) & 1;
        for (uint32_t m = 0; m < count; ++m) WriteReg(one_reg ? index : index + m, r.Read());
        return true;
      }
      case 1: {
        if (r.left < 2) {
          LogTruncated(packet, 2, r.left);
          return false;
        }
        const uint32_t a = r.Read(), b = r.Read();
        WriteReg(packet & 0x7FF, a);
        WriteReg((packet >> 11) & 0x7FF, b);
        return true;
      }
      case 2:
        return true;
      default:
        return Type3(r, packet, depth);
    }
  }

  void Unknown(Reader& r, uint32_t opcode, uint32_t count, const char* why) {
    r.Skip(count);
    ++st.counters.unknown_packets;
    ++st.unknown_by_opcode[opcode];
    kernel::stats::NoteGpuUnknownPacket(opcode);
    if (FirstTime(opcode))
      LogLine("[gpu] PM4 type 3 packet opcode " + Hex(opcode) + " (" + std::to_string(count) + " DWORDs) " + why);
  }

  bool Type3(Reader& r, uint32_t packet, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (r.left < count) {
      LogTruncated(packet, count, r.left);
      return false;
    }
    const uint32_t end_left = r.left - count;
    // Predicated: executed only if a selected bin is in the mask; predicated
    // swaps are always skipped (Xenia).
    if ((packet & 1) && ((st.bin_select & st.bin_mask) == 0 || opcode == kPm4XeSwap)) {
      r.Skip(count);
      return true;
    }
    // Minimum DWORDs read by each handler.
    auto need = [&](uint32_t n) { return count >= n; };
    switch (opcode) {
      case kPm4MeInit:
      case kPm4Nop:
        r.Skip(count);
        break;
      case kPm4Interrupt: {
        const uint32_t cpu_mask = r.Read();
        for (uint32_t n = 0; n < 6; ++n)
          if ((cpu_mask & (1u << n)) && fx.interrupt) fx.interrupt(n);
        break;
      }
      case kPm4XeSwap: {
        if (!need(4)) return Unknown(r, opcode, count, "too short, skipped"), true;
        r.Read();  // kSwapSignature
        const uint32_t fb = r.Read(), w = r.Read(), h = r.Read();
        ++st.counters.frames;
        if (fx.swap) fx.swap(fb, w, h);
        break;
      }
      case kPm4IndirectBuffer:
      case kPm4IndirectBufferPfd: {
        if (!need(2)) return Unknown(r, opcode, count, "too short, skipped"), true;
        const uint32_t list = r.Read() & 0x1FFFFFFF;  // Xenia CpuToGpu
        const uint32_t length = r.Read() & 0xFFFFF;
        if (depth >= kMaxIndirectDepth) {
          static std::atomic<bool> logged{false};
          if (!logged.exchange(true)) LogLine("[gpu] indirect buffers nested beyond the limit: skipped");
          break;
        }
        if (length) {
          Reader ib{base, PhysicalToGuest(list), length, 0, length};
          Execute(ib, depth + 1);  // a truncated IB does not stop the caller (Xenia)
        }
        break;
      }
      case kPm4WaitRegMem: {
        if (!need(5)) return Unknown(r, opcode, count, "too short, skipped"), true;
        const uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read(), wait = r.Read();
        const bool memory = info & 0x10;
        for (;;) {
          uint32_t value;
          if (memory) {
            value = GpuSwap(LoadPhys(poll & ~3u), poll & 3);
          } else {
            if (poll == kRegCoherStatusHost) MakeCoherent();
            value = ReadReg(poll);
          }
          if (Compare(info, value & mask, ref)) break;
          if (!Running()) break;
          // Xenia: waits >= 0x100 sleep wait/0x100 ms, the others yield.
          // Deviation: it sleeps in steps of at most 1 ms with running()
          // checked at every step (wait up to ~65 s): shutdown interrupts
          // the wait immediately.
          if (wait >= 0x100)
            SleepInterruptible(std::chrono::milliseconds(wait / 0x100));
          else
            std::this_thread::yield();
        }
        break;
      }
      case kPm4RegRmw: {
        if (!need(3)) return Unknown(r, opcode, count, "too short, skipped"), true;
        const uint32_t info = r.Read(), and_mask = r.Read(), or_mask = r.Read();
        uint32_t value = ReadReg(info & 0x1FFF);
        value &= (info >> 31) & 1 ? ReadReg(and_mask & 0x1FFF) : and_mask;
        value |= (info >> 30) & 1 ? ReadReg(or_mask & 0x1FFF) : or_mask;
        WriteReg(info & 0x1FFF, value);
        break;
      }
      case kPm4RegToMem: {
        if (!need(2)) return Unknown(r, opcode, count, "too short, skipped"), true;
        const uint32_t reg = r.Read(), addr = r.Read();
        StorePhys(addr & ~3u, GpuSwap(ReadReg(reg), addr & 3));
        break;
      }
      case kPm4MemWrite: {
        if (!need(1)) return Unknown(r, opcode, count, "too short, skipped"), true;
        uint32_t addr = r.Read();
        for (uint32_t i = 1; i < count; ++i) {
          StorePhys(addr & ~3u, GpuSwap(r.Read(), addr & 3));
          addr += 4;
        }
        break;
      }
      case kPm4CondWrite: {
        if (!need(6)) return Unknown(r, opcode, count, "too short, skipped"), true;
        const uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read(), target = r.Read(),
                       data = r.Read();
        const uint32_t value = (info & 0x10) ? GpuSwap(LoadPhys(poll & ~3u), poll & 3) : ReadReg(poll);
        if (Compare(info, value & mask, ref)) {
          if (info & 0x100)
            StorePhys(target & ~3u, GpuSwap(data, target & 3));
          else
            WriteReg(target, data);
        }
        break;
      }
      case kPm4EventWrite: {
        // Xenia: only the initiator; with more DWORDs (memory write) not
        // implemented there either, the rest is skipped.
        const uint32_t initiator = r.Read();
        WriteReg(kRegVgtEventInitiator, initiator & 0x3F);
        break;
      }
      case kPm4EventWriteShd: {
        if (!need(3)) return Unknown(r, opcode, count, "too short, skipped"), true;
        const uint32_t initiator = r.Read(), addr = r.Read(), value = r.Read();
        WriteReg(kRegVgtEventInitiator, initiator & 0x3F);
        const uint32_t data = (initiator >> 31) & 1 ? (fx.counter ? fx.counter() : 0) : value;
        StorePhys(addr & ~3u, GpuSwap(data, addr & 3));
        break;
      }
      case kPm4EventWriteExt: {
        if (!need(2)) return Unknown(r, opcode, count, "too short, skipped"), true;
        const uint32_t initiator = r.Read(), addr = r.Read();
        WriteReg(kRegVgtEventInitiator, initiator & 0x3F);
        // Fake screen extents (Xenia): min/max x, y, z as big-endian u16.
        const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
        uint8_t* p = Phys(addr & ~3u);
        for (int i = 0; i < 6; ++i) {
          const uint16_t be = std::byteswap(extents[i]);
          std::memcpy(p + i * 2, &be, 2);
        }
        break;
      }
      case kPm4EventWriteZpd: {
        const uint32_t initiator = r.Read();
        WriteReg(kRegVgtEventInitiator, initiator & 0x3F);
        // Occlusion query (Xenia): at the end of a query (marker 0xFFFFFEED
        // written by D3D in ZPass_A/B or ZFail_A/B) fake samples; little-endian
        // structure of 8 DWORDs at the RB_SAMPLE_COUNT_ADDR address.
        uint8_t* p = Phys(ReadReg(kRegRbSampleCountAddr));
        uint32_t c[8];
        std::memcpy(c, p, sizeof(c));
        const uint32_t finished = std::byteswap(0xFFFFFEEDu);  // bytes FF FF FE ED read as LE
        const bool end_z_pass = c[4] == finished && c[5] == finished;
        const bool end_z_fail = c[2] == finished && c[3] == finished;
        std::memset(c, 0, sizeof(c));
        if (end_z_pass || end_z_fail) c[0] = c[4] = kFakeSampleCount;  // Total_A, ZPass_A
        std::memcpy(p, c, sizeof(c));
        break;
      }
      case kPm4SetBinMaskLo:
        st.bin_mask = (st.bin_mask & 0xFFFFFFFF00000000ull) | r.Read();
        break;
      case kPm4SetBinMaskHi:
        st.bin_mask = (st.bin_mask & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
        break;
      case kPm4SetBinSelectLo:
        st.bin_select = (st.bin_select & 0xFFFFFFFF00000000ull) | r.Read();
        break;
      case kPm4SetBinSelectHi:
        st.bin_select = (st.bin_select & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
        break;
      case kPm4SetBinMask:
      case kPm4SetBinSelect: {
        if (!need(2)) return Unknown(r, opcode, count, "too short, skipped"), true;
        const uint64_t hi = r.Read(), lo = r.Read();
        (opcode == kPm4SetBinMask ? st.bin_mask : st.bin_select) = (hi << 32) | lo;
        break;
      }
      default:
        Unknown(r, opcode, count, "not executed: skipped");
        return true;
    }
    // DWORDs not read by the handler (Xenia skips them only in some cases).
    if (r.left > end_left) r.Skip(r.left - end_left);
    return true;
  }
};

}  // namespace

uint32_t ExecuteRing(uint8_t* base, uint32_t ring_guest, uint32_t ring_size_dwords, uint32_t read, uint32_t write,
                     const Pm4Effects& fx) {
  if (!ring_size_dwords) return write;
  read %= ring_size_dwords;
  write %= ring_size_dwords;
  Pm4State local;
  Interp in{base, fx, fx.state ? *fx.state : local};
  Reader r{base, ring_guest, ring_size_dwords, read, (write + ring_size_dwords - read) % ring_size_dwords};
  in.Execute(r, 0);
  return write;
}

}  // namespace gpu_fake
