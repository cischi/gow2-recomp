// Basic Rtl/Ke/Hal utilities of the HLE Xbox 360 kernel.
//
// Each function cites its Xenia counterpart
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD),
// src/xenia/kernel/xboxkrnl/*.cc. Deviations are noted case by case.
#include "kernel/rtl_exports.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <ratio>
#include <string>
#include <mutex>
#include <string_view>
#include <vector>

#include "kernel/registry.h"
#include "kernel/stats.h"
#include "kernel/xam.h"
#include "log.h"
#include "platform/platform.h"

namespace {

constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusBufferOverflow = 0x80000005;
constexpr uint32_t kStatusNoMemory = 0xC0000017;
constexpr uint32_t kStatusInvalidParameter2 = 0xC00000F0;

// The kernel's ANSI_STRING / UNICODE_STRING: Length and MaximumLength in bytes.
struct GuestString {
  be<uint16_t> length;
  be<uint16_t> maximum_length;
  be<uint32_t> pointer;
};
static_assert(sizeof(GuestString) == 8);

struct TimeFields {
  be<uint16_t> year, month, day, hour, minute, second, milliseconds, weekday;
};
static_assert(sizeof(TimeFields) == 16);

std::atomic<kernel::rtl::SystemAllocFn> g_alloc{nullptr};
std::atomic<kernel::rtl::SystemFreeFn> g_free{nullptr};
std::atomic<uint32_t> g_system_flags{0};
std::atomic<void (*)()> g_before_exit{nullptr};

// ---- controlled process exit ------------------------------------------

void WriteSummaryAndExit(int code) {
  kernel::stats::WriteSummary();  // at most once, even with several threads exiting
  if (const auto fn = g_before_exit.load()) fn();
  // LogLine has already flushed to file and stderr; _Exit runs no atexit/destructors.
  std::_Exit(code);
}

std::string Hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
  return buf;
}

// Xenia: KeBugCheckEx_entry (xboxkrnl_debug.cc) logs "*** STOP" and stops in the
// debugger; here the process terminates with exit 4 (controlled exit).
// `caller` is the guest's return address (lr).
[[noreturn]] void BugCheck(uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4, uint32_t caller) {
  LogLine("[kernel] KeBugCheck code=" + Hex(code) + " caller=" + Hex(caller) + " params=" + Hex(p1) + "," +
          Hex(p2) + "," + Hex(p3) + "," + Hex(p4));
  WriteSummaryAndExit(4);
  std::abort();
}

}  // namespace

// KeBugCheck/KeBugCheckEx need the lr register for the caller: they do not go
// through function.h's marshalling, which only sees r3..r10.
// Xenia: KeBugCheck_entry -> KeBugCheckEx_entry(code, 0, 0, 0, 0).
KERNEL_EXPORT_RAW(KeBugCheck) { BugCheck(ctx.r3.u32, 0, 0, 0, 0, uint32_t(ctx.lr)); }
KERNEL_EXPORT_RAW(KeBugCheckEx) {
  BugCheck(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, uint32_t(ctx.lr));
}

namespace kernel::rtl {
void SetSystemHeap(SystemAllocFn alloc, SystemFreeFn free) {
  g_alloc = alloc;
  g_free = free;
}
void SetExecutableSystemFlags(uint32_t flags) { g_system_flags = flags; }
void SetBeforeExit(void (*fn)()) { g_before_exit = fn; }

// Made non-decreasing even if the system clock is set back.
uint64_t QuerySystemTime() {
  constexpr int64_t kUnixEpoch = 116444736000000000;  // 1970-01-01 as FILETIME
  static std::atomic<uint64_t> last{0};
  namespace ch = std::chrono;
  const int64_t now100ns = ch::duration_cast<ch::duration<int64_t, std::ratio<1, 10000000>>>(
                               ch::system_clock::now().time_since_epoch())
                               .count();
  uint64_t now = static_cast<uint64_t>(kUnixEpoch + now100ns);
  uint64_t prev = last.load();
  while (now > prev && !last.compare_exchange_weak(prev, now)) {
  }
  return now < prev ? prev : now;
}

}  // namespace kernel::rtl

namespace {

// Xenia: HalReturnToFirmware_entry (xboxkrnl_hal.cc): logs and exit(0).
void HalReturnToFirmwareImpl(uint32_t routine) {
  LogLine("[kernel] HalReturnToFirmware routine=" + std::to_string(routine));
  WriteSummaryAndExit(0);
}

// ---- strings ----------------------------------------------------------

// Xenia: RtlInitAnsiString_entry (xboxkrnl_rtl.cc). Null source: zeroed
// string; empty string: Length 0, MaximumLength 1.
void RtlInitAnsiStringImpl(GuestString* dest, const char* source) {
  if (!dest) return;
  if (source) {
    const uint16_t length = static_cast<uint16_t>(std::min<size_t>(std::strlen(source), 0xFFFF));
    dest->length = length;
    dest->maximum_length = static_cast<uint16_t>(length + 1);  // 0xFFFF+1 -> 0, like the original
  } else {
    dest->length = 0;
    dest->maximum_length = 0;
  }
  dest->pointer = kernel::GuestAddr(source);
}

// Xenia: RtlInitUnicodeString_entry. The source is big-endian UTF-16.
void RtlInitUnicodeStringImpl(GuestString* dest, const be<uint16_t>* source) {
  if (!dest) return;
  if (source) {
    size_t n = 0;
    while (n < 0x7FFF && source[n] != 0) ++n;  // length is u16 in bytes: max 0xFFFE
    dest->length = static_cast<uint16_t>(n * 2);
    dest->maximum_length = static_cast<uint16_t>((n + 1) * 2);
    dest->pointer = kernel::GuestAddr(source);
  } else {
    dest->length = 0;
    dest->maximum_length = 0;
    dest->pointer = 0;
  }
}

// Xenia: RtlFreeAnsiString_entry: frees the buffer in the system heap and
// zeroes the structure.
void RtlFreeAnsiStringImpl(GuestString* s) {
  if (!s) return;
  if (s->pointer != 0) {
    if (auto f = g_free.load()) f(s->pointer);
  }
  s->length = 0;
  s->maximum_length = 0;
  s->pointer = 0;
}

// UTF-16 -> UTF-8 conversion. Xenia: RtlUnicodeStringToAnsiString_entry uses
// xe::to_utf8, so a non-ASCII character becomes its multibyte
// UTF-8 sequence (not '?'); a valid surrogate pair becomes a single code
// point, an unpaired surrogate U+FFFD.
std::string Utf16BeToUtf8(const be<uint16_t>* s, size_t count) {
  std::string out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < count && uint32_t(s[i + 1]) >= 0xDC00 &&
        uint32_t(s[i + 1]) <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t(s[i + 1]) - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

// Xenia: RtlUnicodeStringToAnsiString_entry. Deviation: Xenia does not update
// Length in the no-allocation branch (the real kernel does): here it is updated.
uint32_t RtlUnicodeStringToAnsiStringImpl(GuestString* dest, const GuestString* source, uint32_t alloc_dest) {
  if (!dest || !source) return kStatusInvalidParameter2;
  const auto* chars = kernel::GuestPtr<const be<uint16_t>>(source->pointer);
  const std::string ansi = chars ? Utf16BeToUtf8(chars, source->length / 2) : std::string();
  if (ansi.size() > 0xFFFF - 1) return kStatusInvalidParameter2;

  if (alloc_dest & 0xFF) {  // BOOLEAN
    const auto alloc = g_alloc.load();
    const uint32_t buf = alloc ? alloc(uint32_t(ansi.size() + 1)) : 0;
    if (!buf) return kStatusNoMemory;
    std::memcpy(kernel::GuestPtr<char>(buf), ansi.c_str(), ansi.size() + 1);
    dest->length = static_cast<uint16_t>(ansi.size());
    dest->maximum_length = static_cast<uint16_t>(ansi.size() + 1);
    dest->pointer = buf;
    return kStatusSuccess;
  }

  const uint32_t capacity = dest->maximum_length;
  char* buf = kernel::GuestPtr<char>(dest->pointer);
  if (!buf || capacity == 0) return kStatusBufferOverflow;
  uint32_t result = kStatusSuccess;
  uint32_t written = uint32_t(ansi.size());
  if (capacity < ansi.size() + 1) {
    result = kStatusBufferOverflow;  // writes what fits
    written = capacity - 1;
  }
  std::memcpy(buf, ansi.data(), written);
  buf[written] = 0;
  dest->length = static_cast<uint16_t>(written);
  return result;
}

uint8_t UpperAscii(uint8_t c) { return (c >= 'a' && c <= 'z') ? uint8_t(c - 0x20) : c; }

// Xenia: RtlCompareString_entry compares two C strings with strcmp; the
// real kernel (and the prototype) receives two ANSI_STRINGs and compares by
// length, as here: difference of the first differing byte, otherwise
// difference of the lengths.
int32_t RtlCompareStringImpl(const GuestString* a, const GuestString* b, uint32_t case_insensitive) {
  if (!a || !b) return 0;
  const auto* pa = kernel::GuestPtr<const uint8_t>(a->pointer);
  const auto* pb = kernel::GuestPtr<const uint8_t>(b->pointer);
  const uint32_t la = a->length, lb = b->length;
  const uint32_t n = std::min(la, lb);
  for (uint32_t i = 0; pa && pb && i < n; ++i) {
    int ca = pa[i], cb = pb[i];
    if (case_insensitive & 0xFF) {
      ca = UpperAscii(uint8_t(ca));
      cb = UpperAscii(uint8_t(cb));
    }
    if (ca != cb) return ca - cb;
  }
  return int32_t(la) - int32_t(lb);
}

// ---- memory -----------------------------------------------------------

// Xenia: RtlCompareMemory_entry: number of equal bytes (not the common
// prefix, as in Xenia's original).
uint32_t RtlCompareMemoryImpl(const uint8_t* s1, const uint8_t* s2, uint32_t length) {
  uint32_t c = 0;
  for (uint32_t n = 0; n < length; ++n) c += s1[n] == s2[n];
  return c;
}

// Xenia: RtlCompareMemoryUlong_entry: 0 if address or length is not aligned
// to 4; otherwise number of words equal to the pattern (big-endian in memory).
uint32_t RtlCompareMemoryUlongImpl(const be<uint32_t>* source, uint32_t length, uint32_t pattern) {
  if (kernel::GuestAddr(source) % 4 || length % 4) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < length / 4; ++i) n += uint32_t(source[i]) == pattern;
  return n;
}

// Xenia: RtlFillMemoryUlong_entry (length / 4 words; the remainder is ignored).
void RtlFillMemoryUlongImpl(be<uint32_t>* dest, uint32_t length, uint32_t pattern) {
  for (uint32_t i = 0; i < length / 4; ++i) dest[i] = pattern;
}

// ---- errors -----------------------------------------------------------

struct error_lookup_table {
  uint32_t base_code;
  size_t count;
  const uint32_t* entries;
};
#define MAKE_ENTRY(x) \
  { x, std::size(error_table_##x), error_table_##x }
#include "kernel/nt_status_table.inc"

// Xenia: xeRtlNtStatusToDosError + RtlNtStatusToDosError_entry
// (xboxkrnl_error.cc). Without a table entry: 317 (ERROR_MR_MID_NOT_FOUND).
uint32_t RtlNtStatusToDosErrorImpl(uint32_t status) {
  if (!status || (status & 0x20000000)) return status;
  if ((status >> 16) == 0x8007) return status & 0xFFFF;
  if ((status & 0xF0000000) == 0xD0000000) status &= ~0x30000000;
  for (const auto* t = &error_tables[0]; t->base_code; ++t) {
    if (status < t->base_code) break;
    const uint32_t index = status - t->base_code;
    if (index < t->count) {
      const uint32_t result = t->entries[index];
      if (!result) break;
      return result;
    }
  }
  if ((status >> 16) == 0xC001) return status & 0xFFFF;
  return 317;
}

// ---- time -------------------------------------------------------------

constexpr int64_t kFiletimeUnixEpoch = 116444736000000000;  // 1970-01-01 as FILETIME

int64_t FloorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0 && (a < 0)) ? 1 : 0); }

// Xenia: RtlTimeToTimeFields_entry (xboxkrnl_rtl.cc), system clock
// without scaling: FILETIME (100 ns since 1601) -> calendar fields.
void RtlTimeToTimeFieldsImpl(const be<uint64_t>* time_ptr, TimeFields* f) {
  if (!time_ptr || !f) return;
  namespace ch = std::chrono;
  const int64_t ft = static_cast<int64_t>(uint64_t(*time_ptr));
  const int64_t ms = FloorDiv(ft - kFiletimeUnixEpoch, 10000);
  const int64_t days = FloorDiv(ms, 86400000);
  const int64_t ms_of_day = ms - days * 86400000;
  const ch::sys_days day_point{ch::days{days}};
  const ch::year_month_day ymd{day_point};
  const ch::weekday wd{day_point};
  f->year = static_cast<uint16_t>(int(ymd.year()));
  f->month = static_cast<uint16_t>(unsigned(ymd.month()));
  f->day = static_cast<uint16_t>(unsigned(ymd.day()));
  f->weekday = static_cast<uint16_t>(wd.c_encoding());
  f->hour = static_cast<uint16_t>(ms_of_day / 3600000);
  f->minute = static_cast<uint16_t>(ms_of_day / 60000 % 60);
  f->second = static_cast<uint16_t>(ms_of_day / 1000 % 60);
  f->milliseconds = static_cast<uint16_t>(ms_of_day % 1000);
}

// Xenia: RtlTimeFieldsToTime_entry: 1 if the fields are valid, 0 otherwise.
uint32_t RtlTimeFieldsToTimeImpl(const TimeFields* f, be<uint64_t>* time_ptr) {
  namespace ch = std::chrono;
  if (!f || !time_ptr) return 0;
  if (f->year < 1601 || f->month < 1 || f->month > 12 || f->day < 1 || f->day > 31 || f->hour > 23 ||
      f->minute > 59 || f->second > 59 || f->milliseconds > 999)
    return 0;
  const ch::year_month_day ymd{ch::year{int(uint16_t(f->year))}, ch::month{unsigned(uint16_t(f->month))},
                               ch::day{unsigned(uint16_t(f->day))}};
  if (!ymd.ok()) return 0;
  const int64_t days = ch::sys_days{ymd}.time_since_epoch().count();
  const int64_t ms = ((int64_t(uint16_t(f->hour)) * 60 + uint16_t(f->minute)) * 60 + uint16_t(f->second)) * 1000 +
                     uint16_t(f->milliseconds);
  *time_ptr = static_cast<uint64_t>(kFiletimeUnixEpoch + (days * 86400000 + ms) * 10000);
  return 1;
}

// Xenia: KeQuerySystemTime_entry (Clock::QueryGuestSystemTime): FILETIME
// of the current time (kernel::rtl::QuerySystemTime).
void KeQuerySystemTimeImpl(be<uint64_t>* time_ptr) {
  const uint64_t now = kernel::rtl::QuerySystemTime();
  if (time_ptr) *time_ptr = now;
}

// Xenia: KeQueryPerformanceFrequency_entry (Clock::guest_tick_frequency, 50 MHz).
uint32_t KeQueryPerformanceFrequencyImpl() { return 50'000'000; }

// Xenia: KeGetCurrentProcessType_entry: default process X_PROCTYPE_USER (1).
uint32_t KeGetCurrentProcessTypeImpl() { return 1; }

// Xenia: XexCheckExecutablePrivilege_entry (xboxkrnl_modules.cc): the privilege
// is the position of a bit in the XEX header's system flags
// (XEX_HEADER_SYSTEM_FLAGS); 0 if the bit is not set.
uint32_t XexCheckExecutablePrivilegeImpl(uint32_t privilege) {
  if (privilege > 31) return 0;
  return (g_system_flags.load() & (1u << privilege)) != 0;
}

// ---- debug ------------------------------------------------------------

// Xenia: DbgPrint_entry (xboxkrnl_strings.cc) formats with the variadic
// arguments and logs the text without trailing whitespace. Here: r4..r10 as
// 32-bit integers (%d %i %u %x %X %p %c, %s = guest pointer), no floats.
uint32_t DbgPrintImpl(const char* fmt, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                      uint32_t a6, uint32_t a7) {
  if (!fmt) return kStatusInvalidParameter;
  const uint32_t args[7] = {a1, a2, a3, a4, a5, a6, a7};
  size_t next = 0;
  std::string out;
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') {
      out += *p;
      continue;
    }
    ++p;
    if (*p == '%') {
      out += '%';
      continue;
    }
    std::string spec = "%";
    while (*p && std::strchr("-+ #0123456789.", *p)) spec += *p++;
    while (*p && std::strchr("hlLqjzt", *p)) ++p;  // integers are always 32 bit
    if (!*p) break;
    const uint32_t v = next < 7 ? args[next] : 0;
    ++next;
    char buf[512];
    switch (*p) {
      case 'd': case 'i': std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), int32_t(v)); break;
      case 'u': case 'x': case 'X': case 'o': std::snprintf(buf, sizeof(buf), (spec + *p).c_str(), v); break;
      case 'c': std::snprintf(buf, sizeof(buf), (spec + "c").c_str(), int(v & 0xFF)); break;
      case 'p': std::snprintf(buf, sizeof(buf), "%08X", v); break;
      case 's': {
        const char* s = kernel::GuestPtr<const char>(v);
        std::snprintf(buf, sizeof(buf), (spec + "s").c_str(), s ? s : "(null)");
        break;
      }
      default: std::snprintf(buf, sizeof(buf), "%s%c", spec.c_str(), *p); break;
    }
    out += buf;
  }
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  LogLine("[kernel] DbgPrint: " + out);
  return kStatusSuccess;
}

// ---- length-bounded string conversions ---------------------------------

// Xenia: RtlMultiByteToUnicodeN_entry (xboxkrnl_rtl.cc): copies bytes -> big-endian
// UTF-16, min(dest_len / 2, source_len) characters; *written in bytes.
uint32_t RtlMultiByteToUnicodeNImpl(uint32_t dest, uint32_t dest_len, be<uint32_t>* written, uint32_t source,
                                    uint32_t source_len) {
  const uint32_t n = std::min(dest_len >> 1, source_len);
  auto* d = kernel::GuestPtr<be<uint16_t>>(dest);
  const auto* s = kernel::GuestPtr<const uint8_t>(source);
  if (n && d && s)
    for (uint32_t i = 0; i < n; ++i) d[i] = s[i];
  if (written) *written = n << 1;
  return kStatusSuccess;
}

// Xenia: RtlUnicodeToMultiByteN_entry: big-endian UTF-16 -> bytes,
// min(source_len / 2, dest_len) characters; beyond 0xFF -> '?'; *written in bytes.
uint32_t RtlUnicodeToMultiByteNImpl(uint32_t dest, uint32_t dest_len, be<uint32_t>* written, uint32_t source,
                                    uint32_t source_len) {
  const uint32_t n = std::min(source_len >> 1, dest_len);
  auto* d = kernel::GuestPtr<uint8_t>(dest);
  const auto* s = kernel::GuestPtr<const be<uint16_t>>(source);
  if (n && d && s)
    for (uint32_t i = 0; i < n; ++i) {
      const uint16_t c = s[i];
      d[i] = c < 256 ? uint8_t(c) : uint8_t('?');
    }
  if (written) *written = n;
  return kStatusSuccess;
}

// ---- console configuration (XConfig) -------------------------------------

// Xenia: xeExGetXConfigSetting / ExGetXConfigSetting_entry (xboxkrnl_xconfig.cc).
// Category 2 (secured): AV region USA/Canada. Category 3 (user): time
// zone 0, language = XGetLanguage's (--language), video flags
// 0x00040000, retail flags 0, country 103 (US, one byte). Other: parameter 1 or 2
// invalid. Null buffer with size != 0: parameter 3 invalid.
uint32_t ExGetXConfigSettingImpl(uint32_t category, uint32_t setting, uint8_t* buffer, uint32_t buffer_size,
                                 be<uint16_t>* required_size) {
  constexpr uint32_t kStatusInvalidParameter1 = 0xC00000EF;
  constexpr uint32_t kStatusInvalidParameter3 = 0xC00000F1;
  constexpr uint32_t kStatusBufferTooSmall = 0xC0000023;
  category &= 0xFFFF;
  setting &= 0xFFFF;
  buffer_size &= 0xFFFF;
  uint8_t value[4] = {};
  uint16_t size = 4;
  auto put32 = [&](uint32_t v) { *reinterpret_cast<be<uint32_t>*>(value) = v; };
  if (category == 2) {
    if (setting != 2) return kStatusInvalidParameter2;
    put32(0x00001000);  // XCONFIG_SECURED_AV_REGION: USA/Canada
  } else if (category == 3) {
    switch (setting) {
      case 1: case 2: case 3: case 4: case 5: case 6: case 7: put32(0); break;  // time zone
      case 9: put32(kernel::xam::Language()); break;                             // XCONFIG_USER_LANGUAGE
      case 0xA: put32(0x00040000); break;                                        // XCONFIG_USER_VIDEO_FLAGS
      case 0xC: put32(0); break;                                                 // XCONFIG_USER_RETAIL_FLAGS
      case 0xE: size = 1; value[0] = 103; break;                                 // XCONFIG_USER_COUNTRY: US
      default: return kStatusInvalidParameter2;
    }
  } else {
    return kStatusInvalidParameter1;
  }
  if (buffer) {
    if (buffer_size < size) return kStatusBufferTooSmall;
    std::memcpy(buffer, value, size);
  } else if (buffer_size) {
    return kStatusInvalidParameter3;
  }
  if (required_size) *required_size = size;
  return kStatusSuccess;
}

// Xenia: ExRegisterTitleTerminateNotification_entry (xboxkrnl_modules.cc):
// registers/removes the X_EX_TITLE_TERMINATE_REGISTRATION routine
// { routine u32, priority u32, list_entry }. The runtime never closes the
// title in a way that calls them (the exit is _Exit): only the list is kept.
std::mutex g_terminate_mutex;
std::vector<uint32_t> g_terminate_routines;
void ExRegisterTitleTerminateNotificationImpl(const be<uint32_t>* reg, uint32_t create) {
  if (!reg) return;
  const uint32_t routine = reg[0];
  std::lock_guard lock(g_terminate_mutex);
  if (create) {
    g_terminate_routines.push_back(routine);
  } else {
    std::erase(g_terminate_routines, routine);
  }
}

}  // namespace

namespace kernel::rtl {
size_t TitleTerminateNotificationCount() {
  std::lock_guard lock(g_terminate_mutex);
  return g_terminate_routines.size();
}
}  // namespace kernel::rtl

KERNEL_EXPORT(RtlInitAnsiString, RtlInitAnsiStringImpl)
KERNEL_EXPORT(RtlInitUnicodeString, RtlInitUnicodeStringImpl)
KERNEL_EXPORT(RtlFreeAnsiString, RtlFreeAnsiStringImpl)
KERNEL_EXPORT(RtlUnicodeStringToAnsiString, RtlUnicodeStringToAnsiStringImpl)
KERNEL_EXPORT(RtlCompareString, RtlCompareStringImpl)
KERNEL_EXPORT(RtlCompareMemory, RtlCompareMemoryImpl)
KERNEL_EXPORT(RtlCompareMemoryUlong, RtlCompareMemoryUlongImpl)
KERNEL_EXPORT(RtlFillMemoryUlong, RtlFillMemoryUlongImpl)
KERNEL_EXPORT(RtlNtStatusToDosError, RtlNtStatusToDosErrorImpl)
KERNEL_EXPORT(RtlTimeToTimeFields, RtlTimeToTimeFieldsImpl)
KERNEL_EXPORT(RtlTimeFieldsToTime, RtlTimeFieldsToTimeImpl)
KERNEL_EXPORT(KeQuerySystemTime, KeQuerySystemTimeImpl)
KERNEL_EXPORT(KeQueryPerformanceFrequency, KeQueryPerformanceFrequencyImpl)
KERNEL_EXPORT(KeGetCurrentProcessType, KeGetCurrentProcessTypeImpl)
KERNEL_EXPORT(XexCheckExecutablePrivilege, XexCheckExecutablePrivilegeImpl)
KERNEL_EXPORT(HalReturnToFirmware, HalReturnToFirmwareImpl)
KERNEL_EXPORT(DbgPrint, DbgPrintImpl)
KERNEL_EXPORT(RtlMultiByteToUnicodeN, RtlMultiByteToUnicodeNImpl)
KERNEL_EXPORT(RtlUnicodeToMultiByteN, RtlUnicodeToMultiByteNImpl)
KERNEL_EXPORT(ExGetXConfigSetting, ExGetXConfigSettingImpl)
KERNEL_EXPORT(ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotificationImpl)

// Guest timebase read by mftb (PPC_QUERY_TIMEBASE, runtime/ppc_detail.h,
// XenonRecomp patch 0004): 50 MHz as on the console and as declared by
// KeQueryPerformanceFrequency, from the same source as plat::Ticks50MHz
// (rescaled QueryPerformanceCounter). Monotonic and lock-free: the game reads it
// in tight loops.
extern "C" uint64_t gow2_query_timebase() { return plat::Ticks50MHz(); }
