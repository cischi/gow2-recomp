// Stubs for the system services the PC does not provide: network (NetDll_*/XNet*),
// XMA and voice. None opens PC sockets or plays sounds: they answer like
// a console with no network cable nor headset, without blocking.
//
// Policy: network, Live, voice, XAudio and XMA return offline codes or no
// device, without blocking:
//  - network: XNetStartup/WSAStartup succeed (the game can initialize its
//    libraries), XNetGetTitleXnAddr answers XNET_GET_XNADDR_NONE and the Ethernet
//    link is absent; sockets are local and carry no traffic (decision
//    of 2026-10-20, see the section "local sockets"): socket(), bind,
//    options and sends succeed, no data ever arrives (WSAEWOULDBLOCK immediately)
//    and no PC socket is opened; a handle that is not a socket gives
//    WSAENOTSOCK (like Xenia for an unknown socket); WSA events are
//    real (they can be signaled and waited on);
//  - XAudio: no longer here (sub-project 4): client, callbacks and frames in
//    runtime/apu (audio_system.h, audio_exports.cpp);
//  - XMA: registers and contexts in xma.cpp, decoding in runtime/apu/xma_decoder;
//  - voice: no device.
//
// Each function cites Xenia's
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/kernel/xam/xam_net.cc, xboxkrnl/xboxkrnl_audio.cc,
// xboxkrnl_audio_xma.cc, xam/xam_voice.cc. For imports with no counterpart
// in Xenia (NetDll_XNetQosLookup, XNetRegisterKey, getsockopt, WSARecv...)
// the policy above applies.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/objects.h"
#include "kernel/registry.h"
#include "kernel/sync.h"

namespace kernel::stubs {
namespace {

constexpr uint32_t kErrorAccessDenied = 5;
constexpr uint32_t kErrorInvalidHandle = 6;
constexpr uint32_t kErrorInvalidParameter = 87;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kErrorFunctionFailed = 0x65B;
constexpr uint32_t kStatusInvalidParameterNt = 0xC000000D;

constexpr uint32_t kWsaNotSock = 10038;      // WSAENOTSOCK
constexpr uint32_t kWsaNetDown = 10050;      // WSAENETDOWN
constexpr uint32_t kWsaWaitTimeout = 0x102;  // WSA_WAIT_TIMEOUT
constexpr uint32_t kWsaWaitIoCompletion = 0xC0;
constexpr uint32_t kSocketError = 0xFFFFFFFF;  // -1
constexpr uint32_t kXnetGetXnAddrNone = 1;     // XNET_GET_XNADDR_NONE

be<uint32_t>* P32(uint32_t addr) { return GuestPtr<be<uint32_t>>(addr); }

// Last WSA/Win32 error of the thread (Xenia: XThread::GetLastError/SetLastError).
thread_local uint32_t t_last_error = 0;
uint32_t Fail(uint32_t error) {
  t_last_error = error;
  return kSocketError;
}

// ---- network ------------------------------------------------------------------------

// Xenia: NetDll_XNetStartup_entry / NetDll_XNetCleanup_entry: they succeed.
uint32_t NetDllXNetStartupImpl(uint32_t /*caller*/, uint32_t /*params*/) { return 0; }
uint32_t NetDllXNetCleanupImpl(uint32_t /*caller*/, uint32_t /*params*/) { return 0; }

// Xenia: NetDll_XNetRandom_entry: constant bytes 0xBB. Deviation: a buffer
// that exceeds the end of the guest space is rejected (WSAEINVAL).
uint32_t NetDllXNetRandomImpl(uint32_t /*caller*/, uint32_t buffer, uint32_t length) {
  if (!GuestRangeValid(buffer, length)) return 0x2726;  // WSAEINVAL: beyond the guest space
  if (uint8_t* p = GuestPtr<uint8_t>(buffer)) std::memset(p, 0xBB, length);
  return 0;
}

// Xenia: NetDll_WSAStartup_entry. X_WSADATA: version u16, version_high u16,
// description[257], system_status[129], max_sockets u16 @390, max_udpdg u16
// @392, vendor_info_ptr u32 @396 (untouched: some games compare it).
uint32_t NetDllWSAStartupImpl(uint32_t /*caller*/, uint32_t version, uint32_t data) {
  if (uint8_t* p = GuestPtr<uint8_t>(data)) {
    *reinterpret_cast<be<uint16_t>*>(p) = uint16_t(version);
    *reinterpret_cast<be<uint16_t>*>(p + 2) = uint16_t(version);
    p[4] = 0;
    p[261] = 0;
    *reinterpret_cast<be<uint16_t>*>(p + 390) = 100;
    *reinterpret_cast<be<uint16_t>*>(p + 392) = 1024;
  }
  return 0;
}
uint32_t NetDllWSACleanupImpl(uint32_t /*caller*/) { return 0; }
uint32_t NetDllWSAGetLastErrorImpl() { return t_last_error; }
void NetDllWSASetLastErrorImpl(uint32_t error) { t_last_error = error; }

// Xenia: NetDll_XNetGetTitleXnAddr_entry. Deviation: without a network it returns
// XNET_GET_XNADDR_NONE with the XNADDR (36 bytes) zeroed, not the loopback.
uint32_t NetDllXNetGetTitleXnAddrImpl(uint32_t /*caller*/, uint32_t xnaddr) {
  if (uint8_t* p = GuestPtr<uint8_t>(xnaddr)) std::memset(p, 0, 36);
  return kXnetGetXnAddrNone;
}

// Xenia: NetDll_XNetGetEthernetLinkStatus_entry: no link.
uint32_t NetDllXNetGetEthernetLinkStatusImpl(uint32_t /*caller*/) { return 0; }

// Xenia: NetDll_XNetInAddrToString_entry (but it writes the real address, not
// "666.666.666.666"). The IN_ADDR passed by value has its bytes in network order,
// which on the big-endian PowerPC is the numeric value a.b.c.d.
void NetDllXNetInAddrToStringImpl(uint32_t /*caller*/, uint32_t in_addr, uint32_t out, uint32_t size) {
  char* p = GuestPtr<char>(out);
  if (!p || !size) return;
  char text[20];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", in_addr >> 24, (in_addr >> 16) & 0xFF, (in_addr >> 8) & 0xFF,
                in_addr & 0xFF);
  const size_t n = std::min<size_t>(std::strlen(text), size - 1);
  std::memcpy(p, text, n);
  p[n] = 0;
}

// Xenia: NetDll_inet_addr_entry. Only decimal a.b.c.d; empty string -> 0
// (the console uses the old inet_addr), anything else -> INADDR_NONE (-1).
uint32_t NetDllInetAddrImpl(const char* str) {
  if (!str) return kSocketError;
  if (!*str) return 0;
  uint32_t result = 0;
  const char* p = str;
  for (int part = 0; part < 4; ++part) {
    if (*p < '0' || *p > '9') return kSocketError;
    uint32_t v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
      v = v * 10 + uint32_t(*p - '0');
      if (v > 255) return kSocketError;
    }
    result = (result << 8) | v;
    if (part < 3) {
      if (*p != '.') return kSocketError;
      ++p;
    }
  }
  return *p ? kSocketError : result;
}

// Xenia: NetDll_XNetXnAddrToInAddr_entry (stub, 1). Offline: network-unavailable
// error (also for XNetServerToInAddr, which has no counterpart in Xenia).
uint32_t NetDllXNetToInAddrImpl() { return kWsaNetDown; }
// XNetRegisterKey / UnregisterKey / UnregisterInAddr: they succeed, no state.
uint32_t NetDllXNetNoopImpl() { return 0; }

// Allocations in the process heap (Xenia: SystemHeapAlloc).
uint32_t HeapAlloc(uint32_t size) {
  MemorySystem* m = Memory();
  if (!m) return 0;
  const uint32_t a = m->heap.Alloc(size);
  if (a) std::memset(GuestPtr<uint8_t>(a), 0, size);
  return a;
}
bool HeapFree(uint32_t addr) {
  MemorySystem* m = Memory();
  if (!m || !addr || !m->heap.Size(addr)) return false;
  m->heap.Free(addr);
  return true;
}
void SignalEventHandle(uint32_t handle) {
  if (!handle) return;
  if (auto ev = std::dynamic_pointer_cast<Event>(HandleGet(handle))) ev->Set();
}

// Xenia: NetDll_XNetDnsLookup_entry: XNDNS { iStatus; cina; aina[8] } (40
// bytes) with status 1 (error), no address; the event is signaled.
uint32_t NetDllXNetDnsLookupImpl(uint32_t /*caller*/, const char* /*host*/, uint32_t event_handle, uint32_t pdns) {
  if (pdns) {
    const uint32_t dns = HeapAlloc(40);
    if (dns) *P32(dns) = 1;
    *P32(pdns) = dns;
  }
  SignalEventHandle(event_handle);
  return 0;
}
uint32_t NetDllXNetDnsReleaseImpl(uint32_t /*caller*/, uint32_t dns) {
  if (!dns) return kStatusInvalidParameterNt;
  HeapFree(dns);
  return 0;
}

// NetDll_XNetQosLookup: no counterpart in Xenia, like
// NetDll_XNetQosServiceLookup: XNQOS structure with zero results and zero
// pending, event signaled, result 0. Arguments: caller, cxna, apxna,
// apxnkid, apxnkey, cina, aina, adwServiceId, cProbes, dwBitsPerSec,
// dwFlags, hEvent, ppxnqos.
uint32_t NetDllXNetQosLookupImpl(uint32_t /*caller*/, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, uint32_t, uint32_t, uint32_t event_handle, uint32_t pqos) {
  if (pqos) *P32(pqos) = HeapAlloc(64);
  SignalEventHandle(event_handle);
  return 0;
}
uint32_t NetDllXNetQosReleaseImpl(uint32_t /*caller*/, uint32_t qos) {
  if (!qos) return kStatusInvalidParameterNt;
  HeapFree(qos);
  return 0;
}
// Xenia: NetDll_XNetQosListen_entry.
uint32_t NetDllXNetQosListenImpl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  return kErrorFunctionFailed;
}

// WSAGetOverlappedResult and __WSAFDIsSet: FALSE.
uint32_t NetDllFalseImpl() { return 0; }

// ---- local sockets with no traffic (decision of 2026-10-20) -----------------------
//
// The campaign opens socket(AF_INET, SOCK_DGRAM, VDP) for the "listen" match
// (co-op): if it fails, "Error initializing network layer." and no level
// loads. Here sockets are kernel objects with a real handle and minimal
// state (options, bind port, FIONBIO); no PC socket, no
// traffic. Sends succeed and the data is lost (like lost UDP), receives
// never have data and return immediately with WSAEWOULDBLOCK even on a
// blocking socket: nobody will ever send anything and an infinite wait
// would block a game thread. Xenia instead creates a real host socket.
// The object's type is checked on every call: a closed handle or one of
// another object (event, file) gives WSAENOTSOCK, like Xenia for an
// unknown socket.

constexpr uint32_t kWsaEInval = 10022;          // WSAEINVAL
constexpr uint32_t kWsaEFault = 10014;          // WSAEFAULT
constexpr uint32_t kWsaWouldBlock = 10035;      // WSAEWOULDBLOCK
constexpr uint32_t kWsaProtoNoSupport = 10043;  // WSAEPROTONOSUPPORT
constexpr uint32_t kWsaSockTNoSupport = 10044;  // WSAESOCKTNOSUPPORT
constexpr uint32_t kWsaOpNotSupp = 10045;       // WSAEOPNOTSUPP
constexpr uint32_t kWsaAfNoSupport = 10047;     // WSAEAFNOSUPPORT
constexpr uint32_t kWsaAddrInUse = 10048;       // WSAEADDRINUSE
constexpr uint32_t kWsaNetUnreach = 10051;      // WSAENETUNREACH
constexpr uint32_t kWsaNotConn = 10057;         // WSAENOTCONN

constexpr uint32_t kAfInet = 2;
constexpr uint32_t kSockStream = 1, kSockDgram = 2;
constexpr uint32_t kSolSocket = 0xFFFF, kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002;
constexpr uint32_t kDefaultSockBuffer = 65536;
constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;
constexpr uint16_t kFirstEphemeralPort = 49152;
constexpr uint32_t kSockAddrInSize = 16;  // sockaddr_in: family, port (BE), addr, zero[8]

// State shared by all sockets (multiple game threads): options, bound
// ports, ephemeral port counter.
struct NetState {
  std::mutex mutex;
  std::set<uint16_t> bound_ports;
  uint16_t next_port = kFirstEphemeralPort;
};
NetState& Net() {
  static NetState s;
  return s;
}

class Socket final : public KernelObject {
 public:
  explicit Socket(uint32_t type) : type_(type) {}
  // The port is freed even if the handle is closed with NtClose and not with
  // closesocket. Nobody destroys a Socket with NetState's lock held.
  ~Socket() override {
    if (!port_) return;
    std::lock_guard lock(Net().mutex);
    Net().bound_ports.erase(port_);
  }
  // Not waitable: no network event ever arrives.
  bool IsSignaled() const override { return false; }
  void Acquire() override {}
  uint8_t GuestType() const override { return kNotificationEvent; }

  // Fields protected by Net().mutex.
  const uint32_t type_;
  uint16_t port_ = 0;  // 0 = not bound
  bool non_blocking_ = false;
  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> options_;  // (level, optname)
};

std::shared_ptr<Socket> GetSocket(uint32_t handle) { return std::dynamic_pointer_cast<Socket>(HandleGet(handle)); }

// Xenia: NetDll_socket_entry (real host socket). Here a local object.
uint32_t NetDllSocketImpl(uint32_t /*caller*/, uint32_t af, uint32_t type, uint32_t protocol) {
  if (af != kAfInet) return Fail(kWsaAfNoSupport);
  if (type != kSockStream && type != kSockDgram) return Fail(kWsaSockTNoSupport);
  if (protocol != 0 && protocol != 6 && protocol != 17 && protocol != 0xFE) return Fail(kWsaProtoNoSupport);
  const uint32_t handle = HandleCreate(std::make_shared<Socket>(type));
  return handle ? handle : Fail(kWsaNotSock);
}

uint32_t NetDllCloseSocketImpl(uint32_t /*caller*/, uint32_t handle) {
  if (!GetSocket(handle) || !HandleClose(handle)) return Fail(kWsaNotSock);
  return 0;
}

uint32_t NetDllSetSockOptImpl(uint32_t /*caller*/, uint32_t handle, uint32_t level, uint32_t optname, uint32_t optval,
                              uint32_t optlen) {
  auto s = GetSocket(handle);
  if (!s) return Fail(kWsaNotSock);
  const uint8_t* p = GuestPtr<uint8_t>(optval);
  if (optlen && (!p || !GuestRangeValid(optval, optlen))) return Fail(kWsaEFault);
  std::lock_guard lock(Net().mutex);
  s->options_[{level, optname}].assign(p, p + optlen);
  return 0;
}

uint32_t NetDllGetSockOptImpl(uint32_t /*caller*/, uint32_t handle, uint32_t level, uint32_t optname, uint32_t optval,
                              uint32_t optlen_ptr) {
  auto s = GetSocket(handle);
  if (!s) return Fail(kWsaNotSock);
  be<uint32_t>* len = P32(optlen_ptr);
  uint8_t* out = GuestPtr<uint8_t>(optval);
  if (!len || !out) return Fail(kWsaEFault);
  std::vector<uint8_t> value;
  {
    std::lock_guard lock(Net().mutex);
    if (auto it = s->options_.find({level, optname}); it != s->options_.end()) value = it->second;
  }
  if (value.empty()) {
    // Never set: send and receive buffers of 64 KiB, the rest 0.
    const bool buffer = level == kSolSocket && (optname == kSoSndBuf || optname == kSoRcvBuf);
    value.resize(4);
    *reinterpret_cast<be<uint32_t>*>(value.data()) = buffer ? kDefaultSockBuffer : 0;
  }
  if (uint32_t(*len) < value.size() || !GuestRangeValid(optval, uint32_t(value.size()))) return Fail(kWsaEFault);
  std::memcpy(out, value.data(), value.size());
  *len = uint32_t(value.size());
  return 0;
}

uint32_t NetDllBindImpl(uint32_t /*caller*/, uint32_t handle, uint32_t name, uint32_t namelen) {
  auto s = GetSocket(handle);
  if (!s) return Fail(kWsaNotSock);
  if (!name || namelen < 8 || !GuestRangeValid(name, 8)) return Fail(kWsaEFault);
  uint16_t port = *GuestPtr<be<uint16_t>>(name + 2);
  NetState& net = Net();
  std::lock_guard lock(net.mutex);
  if (s->port_) return Fail(kWsaEInval);  // already bound (like Winsock)
  if (port == 0) {
    // Next free ephemeral port, from 49152 to 65535 and then wrapping around.
    for (uint32_t tries = 0; tries < 65536u - kFirstEphemeralPort; ++tries) {
      const uint16_t candidate = net.next_port;
      net.next_port = candidate == 0xFFFF ? kFirstEphemeralPort : uint16_t(candidate + 1);
      if (!net.bound_ports.contains(candidate)) {
        port = candidate;
        break;
      }
    }
    if (port == 0) return Fail(kWsaAddrInUse);
  } else if (net.bound_ports.contains(port)) {
    return Fail(kWsaAddrInUse);
  }
  net.bound_ports.insert(port);
  s->port_ = port;
  return 0;
}

// sockaddr_in with the bind port (big endian) and address 0.
uint32_t NetDllGetSockNameImpl(uint32_t /*caller*/, uint32_t handle, uint32_t name, uint32_t namelen_ptr) {
  auto s = GetSocket(handle);
  if (!s) return Fail(kWsaNotSock);
  uint16_t port;
  {
    std::lock_guard lock(Net().mutex);
    port = s->port_;
  }
  if (!port) return Fail(kWsaEInval);
  be<uint32_t>* len = P32(namelen_ptr);
  if (!name || !len || uint32_t(*len) < kSockAddrInSize || !GuestRangeValid(name, kSockAddrInSize)) return Fail(kWsaEFault);
  uint8_t* p = GuestPtr<uint8_t>(name);
  std::memset(p, 0, kSockAddrInSize);
  *reinterpret_cast<be<uint16_t>*>(p) = uint16_t(kAfInet);
  *reinterpret_cast<be<uint16_t>*>(p + 2) = port;
  *len = kSockAddrInSize;
  return 0;
}

uint32_t NetDllIoctlSocketImpl(uint32_t /*caller*/, uint32_t handle, uint32_t cmd, uint32_t argp) {
  auto s = GetSocket(handle);
  if (!s) return Fail(kWsaNotSock);
  be<uint32_t>* arg = P32(argp);
  if (cmd == kFionbio) {
    if (!arg) return Fail(kWsaEFault);
    std::lock_guard lock(Net().mutex);
    s->non_blocking_ = uint32_t(*arg) != 0;
    return 0;
  }
  if (cmd == kFionread) {  // no data queued
    if (!arg) return Fail(kWsaEFault);
    *arg = 0;
    return 0;
  }
  return Fail(kWsaEInval);
}

uint32_t NetDllListenImpl(uint32_t /*caller*/, uint32_t handle, uint32_t /*backlog*/) {
  auto s = GetSocket(handle);
  if (!s) return Fail(kWsaNotSock);
  return s->type_ == kSockStream ? 0 : Fail(kWsaOpNotSupp);
}

// accept, recv, recvfrom, WSARecv, WSARecvFrom: never data, never waits.
uint32_t NetDllNoDataImpl(uint32_t /*caller*/, uint32_t handle) {
  return Fail(GetSocket(handle) ? kWsaWouldBlock : kWsaNotSock);
}
uint32_t NetDllConnectImpl(uint32_t /*caller*/, uint32_t handle) {
  return Fail(GetSocket(handle) ? kWsaNetUnreach : kWsaNotSock);
}
uint32_t NetDllGetPeerNameImpl(uint32_t /*caller*/, uint32_t handle) {
  return Fail(GetSocket(handle) ? kWsaNotConn : kWsaNotSock);
}
// shutdown, WSAEventSelect, WSACancelOverlappedIO: succeed with no effects.
uint32_t NetDllSocketNoopImpl(uint32_t /*caller*/, uint32_t handle) {
  return GetSocket(handle) ? 0 : Fail(kWsaNotSock);
}

// send(caller, s, buf, len, flags) and sendto(..., to, tolen): len, data discarded.
uint32_t NetDllSendImpl(uint32_t /*caller*/, uint32_t handle, uint32_t /*buf*/, uint32_t len) {
  return GetSocket(handle) ? len : Fail(kWsaNotSock);
}

// Sum of the WSABUFs { len u32, buf u32 } and, if there is a WSAOVERLAPPED
// (hEvent at +16), its event signaled: the operation is already complete.
uint32_t WsaSendDone(uint32_t handle, uint32_t buffers, uint32_t count, uint32_t sent_ptr, uint32_t overlapped) {
  if (!GetSocket(handle)) return Fail(kWsaNotSock);
  if (count && (!buffers || count > 0x10000000 || !GuestRangeValid(buffers, uint64_t(count) * 8))) return Fail(kWsaEFault);
  uint32_t total = 0;
  for (uint32_t i = 0; i < count; ++i) total += *P32(buffers + 8 * i);
  if (be<uint32_t>* sent = P32(sent_ptr)) *sent = total;
  if (overlapped && GuestRangeValid(overlapped, 20)) SignalEventHandle(*P32(overlapped + 16));
  return 0;
}
// WSASend(caller, s, buffers, count, sent*, flags, overlapped, completion).
uint32_t NetDllWSASendImpl(uint32_t /*caller*/, uint32_t handle, uint32_t buffers, uint32_t count, uint32_t sent,
                           uint32_t /*flags*/, uint32_t overlapped) {
  return WsaSendDone(handle, buffers, count, sent, overlapped);
}
// WSASendTo(caller, s, buffers, count, sent*, flags, to, tolen, overlapped, completion).
uint32_t NetDllWSASendToImpl(uint32_t /*caller*/, uint32_t handle, uint32_t buffers, uint32_t count, uint32_t sent,
                             uint32_t /*flags*/, uint32_t /*to*/, uint32_t /*tolen*/, uint32_t overlapped) {
  return WsaSendDone(handle, buffers, count, sent, overlapped);
}

// select(caller, nfds, readfds, writefds, exceptfds, timeout). fd_set:
// fd_count u32, fd_array[64] u32. No socket is ever readable nor in
// error; sockets in the write set all stay ready (the game may wait
// to be able to send). Returns immediately, whatever the timeout.
uint32_t NetDllSelectImpl(uint32_t /*caller*/, uint32_t /*nfds*/, uint32_t readfds, uint32_t writefds,
                          uint32_t exceptfds) {
  if (be<uint32_t>* rd = P32(readfds)) *rd = 0;
  if (be<uint32_t>* ex = P32(exceptfds)) *ex = 0;
  be<uint32_t>* wr = P32(writefds);
  return wr ? uint32_t(*wr) : 0;
}

// ---- WSA events (real) -----------------------------------------------------------------

// Xenia: NetDll_WSACreateEvent_entry (manual-reset event, not signaled).
uint32_t NetDllWSACreateEventImpl() { return HandleCreate(std::make_shared<Event>(true, false)); }
uint32_t NetDllWSACloseEventImpl(uint32_t handle) {
  if (HandleClose(handle)) return 1;
  t_last_error = kErrorInvalidHandle;
  return 0;
}
uint32_t NetDllWSAResetEventImpl(uint32_t handle) {
  if (auto ev = std::dynamic_pointer_cast<Event>(HandleGet(handle))) {
    ev->Reset();
    return 1;
  }
  t_last_error = kErrorInvalidHandle;
  return 0;
}
uint32_t NetDllWSASetEventImpl(uint32_t handle) {
  if (auto ev = std::dynamic_pointer_cast<Event>(HandleGet(handle))) {
    ev->Set();
    return 1;
  }
  t_last_error = kErrorInvalidHandle;
  return 0;
}

// Xenia: NetDll_WSAWaitForMultipleEvents_entry (real wait on the events,
// repeated if woken by an alert). Deviation: like Winsock it returns
// the event index (WSA_WAIT_EVENT_0 + i), WSA_WAIT_TIMEOUT on
// timeout and WSA_WAIT_IO_COMPLETION after an APC; Xenia always returns 0.
// timeout in milliseconds, 0xFFFFFFFF = infinite.
uint32_t NetDllWSAWaitForMultipleEventsImpl(uint32_t num_events, uint32_t events, uint32_t wait_all,
                                            uint32_t timeout_ms, uint32_t alertable) {
  if (num_events == 0 || num_events > kMaximumWaitObjects || !events) {
    t_last_error = kErrorInvalidParameter;
    return kSocketError;
  }
  std::shared_ptr<KernelObject> objects[kMaximumWaitObjects];
  for (uint32_t i = 0; i < num_events; ++i) {
    objects[i] = HandleGet(*P32(events + 4 * i));
    if (!objects[i]) {
      t_last_error = kErrorInvalidHandle;
      return kSocketError;
    }
  }
  const int64_t timeout = -int64_t(timeout_ms) * 10000;
  const int64_t* limit = timeout_ms == 0xFFFFFFFFu ? nullptr : &timeout;
  uint32_t status;
  do {
    status = wait_all ? WaitAll({objects, num_events}, limit, alertable != 0)
                      : WaitAny({objects, num_events}, limit, alertable != 0);
  } while (status == kStatusAlerted);
  if (status == kStatusTimeout) return kWsaWaitTimeout;
  if (status == kStatusUserApc) return kWsaWaitIoCompletion;
  if (status >= kStatusAbandonedWait0) status -= kStatusAbandonedWait0;
  return status < num_events ? status : kSocketError;
}

// ---- XAudio ------------------------------------------------------------------------------

// XAudio* (rendering driver client) live in runtime/apu/audio_exports.cpp.

// ---- XMA -------------------------------------------------------------------------------------

// XMACreateContext / XMAReleaseContext live in xma.cpp, with the registers and
// the decoder's context array.

// ---- voice -------------------------------------------------------------------------------------

// Xenia: XamVoiceCreate_entry: zeroes the handle and denies access (no headset).
uint32_t XamVoiceCreateImpl(uint32_t, uint32_t, uint32_t out_voice) {
  if (be<uint32_t>* p = P32(out_voice)) *p = 0;
  return kErrorAccessDenied;
}
// Xenia: XamVoiceClose_entry / XamVoiceHeadsetPresent_entry (0).
uint32_t XamVoiceZeroImpl(uint32_t /*voice*/) { return 0; }
// XamVoiceSubmitPacket: no counterpart in Xenia; no device.
uint32_t XamVoiceSubmitPacketImpl(uint32_t, uint32_t, uint32_t) { return kErrorDeviceNotConnected; }

}  // namespace
}  // namespace kernel::stubs

KERNEL_EXPORT(NetDll_XNetStartup, kernel::stubs::NetDllXNetStartupImpl)
KERNEL_EXPORT(NetDll_XNetCleanup, kernel::stubs::NetDllXNetCleanupImpl)
KERNEL_EXPORT(NetDll_XNetRandom, kernel::stubs::NetDllXNetRandomImpl)
KERNEL_EXPORT(NetDll_WSAStartup, kernel::stubs::NetDllWSAStartupImpl)
KERNEL_EXPORT(NetDll_WSACleanup, kernel::stubs::NetDllWSACleanupImpl)
KERNEL_EXPORT(NetDll_WSAGetLastError, kernel::stubs::NetDllWSAGetLastErrorImpl)
KERNEL_EXPORT(NetDll_WSASetLastError, kernel::stubs::NetDllWSASetLastErrorImpl)
KERNEL_EXPORT(NetDll_XNetGetTitleXnAddr, kernel::stubs::NetDllXNetGetTitleXnAddrImpl)
KERNEL_EXPORT(NetDll_XNetGetEthernetLinkStatus, kernel::stubs::NetDllXNetGetEthernetLinkStatusImpl)
KERNEL_EXPORT(NetDll_XNetInAddrToString, kernel::stubs::NetDllXNetInAddrToStringImpl)
KERNEL_EXPORT(NetDll_inet_addr, kernel::stubs::NetDllInetAddrImpl)
KERNEL_EXPORT(NetDll_XNetXnAddrToInAddr, kernel::stubs::NetDllXNetToInAddrImpl)
KERNEL_EXPORT(NetDll_XNetServerToInAddr, kernel::stubs::NetDllXNetToInAddrImpl)
KERNEL_EXPORT(NetDll_XNetRegisterKey, kernel::stubs::NetDllXNetNoopImpl)
KERNEL_EXPORT(NetDll_XNetUnregisterKey, kernel::stubs::NetDllXNetNoopImpl)
KERNEL_EXPORT(NetDll_XNetUnregisterInAddr, kernel::stubs::NetDllXNetNoopImpl)
KERNEL_EXPORT(NetDll_XNetDnsLookup, kernel::stubs::NetDllXNetDnsLookupImpl)
KERNEL_EXPORT(NetDll_XNetDnsRelease, kernel::stubs::NetDllXNetDnsReleaseImpl)
KERNEL_EXPORT(NetDll_XNetQosLookup, kernel::stubs::NetDllXNetQosLookupImpl)
KERNEL_EXPORT(NetDll_XNetQosRelease, kernel::stubs::NetDllXNetQosReleaseImpl)
KERNEL_EXPORT(NetDll_XNetQosListen, kernel::stubs::NetDllXNetQosListenImpl)
KERNEL_EXPORT(NetDll_socket, kernel::stubs::NetDllSocketImpl)
KERNEL_EXPORT(NetDll_closesocket, kernel::stubs::NetDllCloseSocketImpl)
KERNEL_EXPORT(NetDll_shutdown, kernel::stubs::NetDllSocketNoopImpl)
KERNEL_EXPORT(NetDll_setsockopt, kernel::stubs::NetDllSetSockOptImpl)
KERNEL_EXPORT(NetDll_getsockopt, kernel::stubs::NetDllGetSockOptImpl)
KERNEL_EXPORT(NetDll_getsockname, kernel::stubs::NetDllGetSockNameImpl)
KERNEL_EXPORT(NetDll_getpeername, kernel::stubs::NetDllGetPeerNameImpl)
KERNEL_EXPORT(NetDll_ioctlsocket, kernel::stubs::NetDllIoctlSocketImpl)
KERNEL_EXPORT(NetDll_bind, kernel::stubs::NetDllBindImpl)
KERNEL_EXPORT(NetDll_connect, kernel::stubs::NetDllConnectImpl)
KERNEL_EXPORT(NetDll_listen, kernel::stubs::NetDllListenImpl)
KERNEL_EXPORT(NetDll_accept, kernel::stubs::NetDllNoDataImpl)
KERNEL_EXPORT(NetDll_select, kernel::stubs::NetDllSelectImpl)
KERNEL_EXPORT(NetDll_recv, kernel::stubs::NetDllNoDataImpl)
KERNEL_EXPORT(NetDll_recvfrom, kernel::stubs::NetDllNoDataImpl)
KERNEL_EXPORT(NetDll_send, kernel::stubs::NetDllSendImpl)
KERNEL_EXPORT(NetDll_sendto, kernel::stubs::NetDllSendImpl)
KERNEL_EXPORT(NetDll_WSARecv, kernel::stubs::NetDllNoDataImpl)
KERNEL_EXPORT(NetDll_WSARecvFrom, kernel::stubs::NetDllNoDataImpl)
KERNEL_EXPORT(NetDll_WSASend, kernel::stubs::NetDllWSASendImpl)
KERNEL_EXPORT(NetDll_WSASendTo, kernel::stubs::NetDllWSASendToImpl)
KERNEL_EXPORT(NetDll_WSAEventSelect, kernel::stubs::NetDllSocketNoopImpl)
KERNEL_EXPORT(NetDll_WSACancelOverlappedIO, kernel::stubs::NetDllSocketNoopImpl)
KERNEL_EXPORT(NetDll_WSAGetOverlappedResult, kernel::stubs::NetDllFalseImpl)
KERNEL_EXPORT(NetDll___WSAFDIsSet, kernel::stubs::NetDllFalseImpl)
KERNEL_EXPORT(NetDll_WSACreateEvent, kernel::stubs::NetDllWSACreateEventImpl)
KERNEL_EXPORT(NetDll_WSACloseEvent, kernel::stubs::NetDllWSACloseEventImpl)
KERNEL_EXPORT(NetDll_WSAResetEvent, kernel::stubs::NetDllWSAResetEventImpl)
KERNEL_EXPORT(NetDll_WSASetEvent, kernel::stubs::NetDllWSASetEventImpl)
KERNEL_EXPORT(NetDll_WSAWaitForMultipleEvents, kernel::stubs::NetDllWSAWaitForMultipleEventsImpl)
KERNEL_EXPORT(XamVoiceCreate, kernel::stubs::XamVoiceCreateImpl)
KERNEL_EXPORT(XamVoiceClose, kernel::stubs::XamVoiceZeroImpl)
KERNEL_EXPORT(XamVoiceHeadsetPresent, kernel::stubs::XamVoiceZeroImpl)
KERNEL_EXPORT(XamVoiceSubmitPacket, kernel::stubs::XamVoiceSubmitPacketImpl)
