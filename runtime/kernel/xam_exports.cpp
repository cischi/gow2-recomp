// XAM exports: local user, content, enumerators, system UI,
// notifications, language and system information.
//
// Each function cites its Xenia counterpart
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/kernel/xam/xam_user.cc, xam_content.cc, xam_content_device.cc,
// xam_enum.cc, xam_notify.cc, xam_ui.cc, xam_info.cc, xam_video.cc;
// src/xenia/kernel/xnotifylistener.cc, xenumerator.cc, kernel_state.cc
// (CompleteOverlapped*, RegisterNotifyListener) and xam/content_manager.cc.
// Deviations are noted case by case. Only the imports the game
// really uses are implemented (games/gow2/expected_xex.json).
//
// Model:
//  - a single user, index 0, authenticated locally (signin state 1), name
//    "Player", offline XUID 0xE000000000000001; indices 1-3 are not
//    authenticated;
//  - a single storage device, id 1 "Hard Drive";
//  - each content opened with XamContentCreateEx is the folder
//    <content root>/<sanitized name> (user_data/content), mounted read/write
//    under the root name chosen by the game until XamContentClose;
//  - system UIs complete immediately with the default result;
//  - async services complete immediately: they write the XOVERLAPPED, signal
//    hEvent, queue the completion routine as an APC of the calling
//    thread and return ERROR_IO_PENDING, like Xenia (without the 100 ms
//    delay of its dispatch thread).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "kernel/guest.h"
#include "kernel/io/vfs.h"
#include "kernel/memory.h"
#include "kernel/objects.h"
#include "kernel/registry.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "kernel/xam.h"
#include "log.h"

namespace fs = std::filesystem;

namespace kernel::xam {
namespace {

// Codes (Xenia src/xenia/xbox.h).
constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorFileNotFound = 2;
constexpr uint32_t kErrorPathNotFound = 3;
constexpr uint32_t kErrorAccessDenied = 5;
constexpr uint32_t kErrorInvalidHandle = 6;
constexpr uint32_t kErrorNoMoreFiles = 0x12;
constexpr uint32_t kErrorInvalidParameter = 0x57;
constexpr uint32_t kErrorInsufficientBuffer = 0x7A;
constexpr uint32_t kErrorAlreadyExists = 0xB7;
constexpr uint32_t kErrorIoIncomplete = 0x3E4;
constexpr uint32_t kErrorIoPending = 0x3E5;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kErrorNotFound = 0x490;
constexpr uint32_t kErrorNotLoggedOn = 0x4DD;
constexpr uint32_t kErrorNoSuchUser = 0x525;
constexpr uint32_t kErrorFunctionFailed = 0x65B;

constexpr uint32_t HResultFromWin32(uint32_t e) { return e ? (0x80070000u | (e & 0xFFFF)) : 0; }
constexpr uint32_t kEInvalidArg = 0x80070057;  // X_E_INVALIDARG
constexpr uint32_t kENoSuchUser = 0x80070525;  // X_E_NO_SUCH_USER
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusNoMemory = 0xC0000017;

// XAM's own app (XMsg app 0xFE, Xenia apps/xam_app.cc) and its message that
// enumerates an XamContentAggregateCreateEnumerator handle.
constexpr uint32_t kXamAppId = 0xFE;
constexpr uint32_t kMsgContentAggregateEnumerate = 0x0002000E;
constexpr uint32_t kXObjSignature = 0x58454E00;  // 'XEN\0' (Xenia xobject.h), magic of the aggregate data

// User: local index 0, stable fictitious XUID and not online.
constexpr uint32_t kUserCount = 4;
constexpr uint64_t kLocalXuid = 0xE000000000000001ull;
constexpr char kUserName[] = "Player";
constexpr uint32_t kSigninLocal = 1;  // XUserSigninState_SignedInLocally

// Single device (Xenia DummyDeviceId::HDD = 1, DeviceType::HDD = 1).
constexpr uint32_t kHddDeviceId = 1;
constexpr uint32_t kHddDeviceType = 1;
constexpr uint64_t kGiB = 1024ull * 1024 * 1024;
constexpr uint64_t kHddTotalBytes = 20 * kGiB, kHddFreeBytes = 3 * kGiB;  // like Xenia
constexpr char16_t kHddName[] = u"Hard Drive";

// Notifications (XN_SYS_*, Xenia kernel_state.cc / SDK): the id is encoded as
// XNotificationKey (xnotifylistener.h): local_id:16, version:9, mask_index:6.
constexpr uint32_t kXnSysUi = 0x00000009;
constexpr uint32_t kXnSysSigninChanged = 0x0000000A;
constexpr uint32_t kXnSysStorageDevicesChanged = 0x0000000B;
constexpr uint32_t kXnSystemArea = 0x00000001;  // bit 0 of the mask: system area

constexpr uint32_t kXContentDataSize = 0x134;  // XCONTENT_DATA
constexpr uint32_t kContentFileNameOffset = 0x108;
constexpr uint32_t kContentFileNameSize = 42;

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", v);
  return buf;
}

be<uint32_t>* P32(uint32_t addr) { return GuestPtr<be<uint32_t>>(addr); }

// ---- XOVERLAPPED ---------------------------------------------------------------

// Xenia: KernelState::CompleteOverlappedEx. XAM_OVERLAPPED: result 0x0, length
// 0x4, context 0x8, event 0xC, completion_routine 0x10, completion_context
// 0x14, extended_error 0x18. Deviation: context (thread handle) is not
// written; the routine goes to the current guest thread, the one that
// called the service, without looking it up by handle.
void CompleteOverlappedEx(uint32_t ov, uint32_t result, uint32_t extended_error, uint32_t length) {
  be<uint32_t>* p = P32(ov);
  if (!p) return;
  p[0] = result;
  p[6] = extended_error;
  p[1] = length;
  if (const uint32_t event = p[3]) {
    auto ev = std::dynamic_pointer_cast<Event>(HandleGet(event));
    if (ev)
      ev->Set();
    else
      LogLine("[kernel] XOVERLAPPED " + Hex(ov) + ": unknown event " + Hex(event) + " (caller " +
              Hex(ExportCaller()) + ")");
  }
  if (const uint32_t routine = p[4]) {
    if (GuestThread* t = GuestThread::Current()) t->QueueApc(routine, result, length, ov);
  }
}

// Xenia: CompleteOverlappedImmediate(Ex): length = 0 on success, 0xFFFFFFFF
// otherwise (some games look at the length as the result).
uint32_t CompleteImmediate(uint32_t ov, uint32_t result) {
  CompleteOverlappedEx(ov, result, result, result ? 0xFFFFFFFFu : 0);
  return kErrorIoPending;
}
uint32_t CompleteImmediateEx(uint32_t ov, uint32_t result, uint32_t extended_error, uint32_t length) {
  CompleteOverlappedEx(ov, result, extended_error, length);
  return kErrorIoPending;
}

// ---- language ------------------------------------------------------------------

std::atomic<uint32_t> g_language{1};

// ---- notification listeners ---------------------------------------------------------

// Xenia: XNotifyListener (xnotifylistener.cc). A KernelObject signaled
// when the queue is not empty (games wait on it); the queue lives
// under the dispatcher lock, like IsSignaled.
class NotifyListener final : public KernelObject {
 public:
  NotifyListener(uint64_t mask, uint32_t max_version) : mask_(mask), max_version_(max_version) {}

  uint64_t mask() const { return mask_; }

  // EnqueueNotification: ignores notifications outside the mask or too new.
  void Enqueue(uint32_t id, uint32_t data) {
    const uint32_t version = (id >> 16) & 0x1FF;
    const uint32_t mask_index = (id >> 25) & 0x3F;
    if ((mask_ & (1ull << mask_index)) == 0) return;
    if (version > max_version_) return;
    auto lock = LockDispatcher();
    queue_.emplace_back(id, data);
    SatisfyWaiters(*this);
  }
  // DequeueNotification(&id, &data) and DequeueNotification(id, &data).
  bool Dequeue(uint32_t match_id, uint32_t* id, uint32_t* data) {
    auto lock = LockDispatcher();
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
      if (match_id && it->first != match_id) continue;
      *id = it->first;
      *data = it->second;
      queue_.erase(it);
      return true;
    }
    return false;
  }

  bool IsSignaled() const override { return !queue_.empty(); }
  void Acquire() override {}
  uint8_t GuestType() const override { return kNotificationEvent; }

 private:
  const uint64_t mask_;
  const uint32_t max_version_;
  std::vector<std::pair<uint32_t, uint32_t>> queue_;  // dispatcher lock
};

std::mutex g_listeners_mutex;
std::vector<std::weak_ptr<NotifyListener>> g_listeners;

// ---- enumerators ---------------------------------------------------------------------

// Xenia: XStaticUntypedEnumerator (xenumerator.cc). Fixed-size items
// copied in blocks of `per_enumerate`. The guest X_KENUMERATOR exists only
// once XamGetPrivateEnumStructureFromHandle asks for it.
class Enumerator final : public KernelObject {
 public:
  // X_KENUMERATOR fields Xenia's XEnumerator::Initialize sets: the title sends
  // open_message to app_id (XMsg) to enumerate through XAM.
  struct Messages {
    uint32_t app_id, open_message, close_message, user_index;  // all 0 for `{}`
  };
  Enumerator(uint32_t item_size, uint32_t per_enumerate, Messages messages = {})
      : item_size_(item_size), per_enumerate_(per_enumerate), messages_(messages) {}
  ~Enumerator() override {
    if (MemorySystem* m = Memory(); m && guest_private_) m->heap.Free(guest_private_);
  }
  // X_KENUMERATOR { app_id, open_message, close_message, user_index,
  // items_per_enumerate, flags } followed by Xenia's aggregate extra data
  // { magic, handle }; allocated on first use, 0 if memory is missing.
  uint32_t GuestPrivate(uint32_t handle) {
    std::lock_guard lock(mutex_);
    if (guest_private_) return guest_private_;
    MemorySystem* m = Memory();
    if (!m || !(guest_private_ = m->heap.Alloc(kSize))) return 0;
    be<uint32_t>* p = GuestPtr<be<uint32_t>>(guest_private_);
    const uint32_t fields[kSize / 4] = {messages_.app_id,     messages_.open_message, messages_.close_message,
                                        messages_.user_index, per_enumerate_,         0,
                                        kXObjSignature,       handle};
    for (uint32_t i = 0; i < kSize / 4; ++i) p[i] = fields[i];
    return guest_private_;
  }
  void Append(const uint8_t* item) { items_.insert(items_.end(), item, item + item_size_); }
  // WriteItems: ERROR_NO_MORE_FILES when the items are exhausted.
  uint32_t WriteItems(uint8_t* out, uint32_t* written) {
    std::lock_guard lock(mutex_);
    const size_t total = items_.size() / item_size_;
    const size_t count = std::min<size_t>(total - std::min(current_, total), per_enumerate_);
    if (!count) return kErrorNoMoreFiles;
    std::memcpy(out, items_.data() + current_ * item_size_, count * item_size_);
    current_ += count;
    *written = uint32_t(count);
    return kErrorSuccess;
  }

  bool IsSignaled() const override { return false; }
  void Acquire() override {}
  uint8_t GuestType() const override { return kNotificationEvent; }

 private:
  static constexpr uint32_t kSize = 32;
  const uint32_t item_size_, per_enumerate_;
  const Messages messages_;
  uint32_t guest_private_ = 0;
  std::vector<uint8_t> items_;
  std::mutex mutex_;
  size_t current_ = 0;
};

// ---- contents ---------------------------------------------------------------------------

struct OpenPackage {
  std::string device;  // "save:" as mounted in the VFS
  std::string name;    // sanitized name (subfolder of the content root)
};

std::mutex g_content_mutex;
fs::path g_content_root;
std::map<std::string, OpenPackage> g_open;  // lowercase root -> package

std::string LowerAscii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

// Safe folder name for the file name of an XCONTENT_DATA: every character
// the VFS does not allow (controls, non-ASCII, `"*<>?|`, `:`, separators)
// becomes '_', and a still-invalid name (".", "..", "CON", "a.") gets a
// trailing '_'. Never separators nor a lone "..": it always stays a single
// component under the content root.
std::string SanitizeContentName(std::string_view raw) {
  std::string s(raw);
  for (char& c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u >= 0x7F || std::strchr("\"*<>?|:\\/", c)) c = '_';
  }
  if (!io::IsValidComponent(s)) s.push_back('_');
  if (!io::IsValidComponent(s)) s = "_";
  return s;
}

struct ContentData {
  uint32_t device_id = 0, content_type = 0;
  std::string raw_name;  // the game's file_name, up to 42 characters
};

bool ReadContentData(uint32_t addr, ContentData* out) {
  if (!addr) return false;
  const uint8_t* p = GuestPtr<uint8_t>(addr);
  out->device_id = *reinterpret_cast<const be<uint32_t>*>(p);
  out->content_type = *reinterpret_cast<const be<uint32_t>*>(p + 4);
  const char* name = reinterpret_cast<const char*>(p + kContentFileNameOffset);
  size_t n = 0;
  while (n < kContentFileNameSize && name[n]) ++n;
  out->raw_name.assign(name, n);
  return true;
}

fs::path PackagePath(const std::string& sanitized) {
  std::lock_guard lock(g_content_mutex);
  return g_content_root / sanitized;
}

bool ContentExists(const ContentData& d) {
  std::error_code ec;
  return fs::is_directory(PackagePath(SanitizeContentName(d.raw_name)), ec);
}

bool ContentIsOpen(const std::string& sanitized) {
  std::lock_guard lock(g_content_mutex);
  for (const auto& [root, pkg] : g_open)
    if (pkg.name == sanitized) return true;
  return false;
}

// Xenia: ContentManager::DeleteContent. ACCESS_DENIED if open.
uint32_t DeleteContent(const ContentData& d) {
  const std::string name = SanitizeContentName(d.raw_name);
  if (ContentIsOpen(name)) return kErrorAccessDenied;
  std::error_code ec;
  const auto removed = fs::remove_all(PackagePath(name), ec);
  return (!ec && removed > 0) ? kErrorSuccess : kErrorFileNotFound;
}

// Xenia: ContentManager::CreateContent / OpenContent (same rules, with
// the folder's existence in place of the STFS package's) plus the
// mounting of the root in the VFS.
uint32_t MountPackage(const std::string& root, const std::string& sanitized, bool create) {
  const fs::path dir = PackagePath(sanitized);
  std::error_code ec;
  if (create) {
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) return kErrorAccessDenied;
  } else if (!fs::is_directory(dir, ec)) {
    return kErrorFileNotFound;
  }
  std::string device = root + ":";
  std::lock_guard lock(g_content_mutex);
  g_open[LowerAscii(root)] = {device, sanitized};
  io::AddMount({device, dir, true});
  return kErrorSuccess;
}

// Xenia: xeXamContentCreate. Deviations: root already open or coinciding
// with a VFS device (game:, cache:...) -> ALREADY_EXISTS before
// deleting anything; invalid root name -> INVALID_PARAMETER; the content
// name is sanitized; the overlapped completion is immediate.
uint32_t ContentCreate(uint32_t root_addr, uint32_t data_addr, uint32_t flags, uint32_t disposition_ptr,
                       uint32_t license_mask_ptr, uint32_t ov) {
  uint32_t result = kErrorInvalidParameter;
  uint32_t disposition = 0;  // 0 unknown, 1 created, 2 opened
  const char* root_c = GuestPtr<const char>(root_addr);
  ContentData d;
  const std::string root = root_c ? std::string(root_c, strnlen(root_c, 256)) : std::string();
  if (root_c && ReadContentData(data_addr, &d) && !d.raw_name.empty() && io::IsValidComponent(root)) {
    const std::string name = SanitizeContentName(d.raw_name);
    bool root_taken;
    {
      std::lock_guard lock(g_content_mutex);
      root_taken = g_open.count(LowerAscii(root)) != 0;
    }
    if (!root_taken && io::Resolve(root + ":")) root_taken = true;  // already existing VFS device
    if (root_taken) {
      result = kErrorAlreadyExists;
    } else {
      enum { kUnknown, kCreate, kOpen } what = kUnknown;
      switch (flags & 0xF) {
        case 1:  // CREATE_NEW: fails if it exists
          if (ContentExists(d))
            result = kErrorAlreadyExists;
          else
            what = kCreate;
          break;
        case 2:  // CREATE_ALWAYS: overwrites
          if (ContentExists(d)) DeleteContent(d);
          what = kCreate;
          break;
        case 3:  // OPEN_EXISTING
          if (!ContentExists(d))
            result = kErrorPathNotFound;
          else
            what = kOpen;
          break;
        case 4:  // OPEN_ALWAYS
          what = ContentExists(d) ? kOpen : kCreate;
          break;
        case 5:  // TRUNCATE_EXISTING: must exist, then deletes and recreates
          if (!ContentExists(d)) {
            result = kErrorPathNotFound;
          } else {
            DeleteContent(d);
            what = kCreate;
          }
          break;
        default:
          break;
      }
      if (what != kUnknown) {
        result = MountPackage(root, name, what == kCreate);
        if (result == kErrorSuccess) disposition = what == kCreate ? 1 : 2;
      }
    }
  }
  if (be<uint32_t>* p = P32(disposition_ptr)) *p = disposition;
  if (result == kErrorSuccess)
    if (be<uint32_t>* p = P32(license_mask_ptr)) *p = 0;  // Xenia: stub, no license
  if (ov) return CompleteImmediateEx(ov, result, HResultFromWin32(result), disposition);
  return result;
}

// Xenia: XamContentCreateEx_entry (xam_content.cc). cache_size and content_size
// (STFS type) are not needed: contents are folders.
uint32_t XamContentCreateExImpl(uint32_t /*user_index*/, uint32_t root_name, uint32_t content_data, uint32_t flags,
                                uint32_t disposition_ptr, uint32_t license_mask_ptr, uint32_t /*cache_size*/,
                                uint64_t /*content_size*/, uint32_t overlapped) {
  return ContentCreate(root_name, content_data, flags, disposition_ptr, license_mask_ptr, overlapped);
}

// Xenia: XamContentFlush_entry: nothing to flush.
uint32_t XamContentFlushImpl(uint32_t /*root_name*/, uint32_t overlapped) {
  if (overlapped) return CompleteImmediate(overlapped, kErrorSuccess);
  return kErrorSuccess;
}

// Xenia: XamContentClose_entry -> ContentManager::CloseContent. Unmounts the
// root from the VFS.
uint32_t XamContentCloseImpl(uint32_t root_name, uint32_t overlapped) {
  uint32_t result = kErrorFileNotFound;
  if (const char* r = GuestPtr<const char>(root_name)) {
    const std::string root(r, strnlen(r, 256));
    std::string device;
    {
      std::lock_guard lock(g_content_mutex);
      auto it = g_open.find(LowerAscii(root));
      if (it != g_open.end()) {
        device = it->second.device;
        g_open.erase(it);
      }
    }
    if (!device.empty()) {
      io::RemoveMount(device);
      result = kErrorSuccess;
    }
  }
  if (overlapped) return CompleteImmediate(overlapped, result);
  return result;
}

// Xenia: XamContentGetCreator_entry. A save (type 1) was always created
// by the user.
uint32_t XamContentGetCreatorImpl(uint32_t /*user_index*/, uint32_t content_data, uint32_t is_creator_ptr,
                                  uint32_t creator_xuid_ptr, uint32_t overlapped) {
  uint32_t result = kErrorSuccess;
  ContentData d;
  if (!ReadContentData(content_data, &d)) {
    result = kErrorInvalidParameter;
  } else if (ContentExists(d)) {
    const bool saved_game = d.content_type == 1;
    if (be<uint32_t>* p = P32(is_creator_ptr)) *p = saved_game ? 1 : 0;
    if (be<uint64_t>* x = GuestPtr<be<uint64_t>>(creator_xuid_ptr)) *x = saved_game ? kLocalXuid : 0;
  } else {
    result = kErrorPathNotFound;
  }
  if (overlapped) return CompleteImmediate(overlapped, result);
  return result;
}

// Xenia: XamContentDelete_entry.
uint32_t XamContentDeleteImpl(uint32_t /*user_index*/, uint32_t content_data, uint32_t overlapped) {
  ContentData d;
  const uint32_t result = ReadContentData(content_data, &d) ? DeleteContent(d) : kErrorInvalidParameter;
  if (overlapped) return CompleteImmediate(overlapped, result);
  return result;
}

// Xenia: XamContentSetThumbnail_entry (ContentManager::SetContentThumbnail,
// file "__thumbnail.png" in the package folder). Size limited to
// 4 MiB (thumbnails are PNGs of a few KB).
uint32_t XamContentSetThumbnailImpl(uint32_t /*user_index*/, uint32_t content_data, uint32_t buffer, uint32_t size,
                                    uint32_t overlapped) {
  uint32_t result = kErrorSuccess;
  ContentData d;
  if (!ReadContentData(content_data, &d) || d.raw_name.empty() || (size && !buffer) || size > (4u << 20)) {
    result = kErrorInvalidParameter;
  } else {
    const fs::path dir = PackagePath(SanitizeContentName(d.raw_name));
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(dir / "__thumbnail.png", std::ios::binary | std::ios::trunc);
    if (ec || !out) {
      result = kErrorFileNotFound;
    } else {
      if (size) out.write(GuestPtr<const char>(buffer), size);
    }
  }
  if (overlapped) return CompleteImmediate(overlapped, result);
  return result;
}

// ---- storage devices -------------------------------------------------------------

// Xenia: XamContentGetDeviceState_entry (xam_content_device.cc). Deviation:
// the only device is id 1 (Xenia also has an optical drive id 2).
uint32_t XamContentGetDeviceStateImpl(uint32_t device_id, uint32_t overlapped) {
  if (device_id != kHddDeviceId) {
    if (overlapped)
      return CompleteImmediateEx(overlapped, kErrorFunctionFailed, kErrorDeviceNotConnected, 0);
    return kErrorDeviceNotConnected;
  }
  if (overlapped) return CompleteImmediate(overlapped, kErrorSuccess);
  return kErrorSuccess;
}

// X_CONTENT_DEVICE_DATA (0x50): id, type, total u64, free u64, name UTF-16[28].
void FillDeviceData(uint8_t* out) {
  std::memset(out, 0, 0x50);
  *reinterpret_cast<be<uint32_t>*>(out) = kHddDeviceId;
  *reinterpret_cast<be<uint32_t>*>(out + 4) = kHddDeviceType;
  *reinterpret_cast<be<uint64_t>*>(out + 8) = kHddTotalBytes;
  *reinterpret_cast<be<uint64_t>*>(out + 16) = kHddFreeBytes;
  for (size_t i = 0; i + 1 < 28 && kHddName[i]; ++i) *reinterpret_cast<be<uint16_t>*>(out + 24 + 2 * i) = kHddName[i];
}

// Xenia: XamContentGetDeviceData_entry.
uint32_t XamContentGetDeviceDataImpl(uint32_t device_id, uint32_t device_data) {
  if (device_id != kHddDeviceId) return kErrorDeviceNotConnected;
  if (uint8_t* out = GuestPtr<uint8_t>(device_data)) FillDeviceData(out);
  return kErrorSuccess;
}

// Xenia: XamContentCreateEnumerator_entry. Deviation: the items are the
// subfolders of the content root (not filtering by content_type, which is not
// part of the path) with device_id 1 and the requested content_type.
uint32_t XamContentCreateEnumeratorImpl(uint32_t /*user_index*/, uint32_t device_id, uint32_t content_type,
                                        uint32_t /*content_flags*/, uint32_t items_per_enumerate,
                                        uint32_t buffer_size_ptr, uint32_t handle_out) {
  if ((device_id && device_id != kHddDeviceId) || !handle_out || !items_per_enumerate) {
    if (be<uint32_t>* p = P32(buffer_size_ptr)) *p = 0;
    return kEInvalidArg;
  }
  if (be<uint32_t>* p = P32(buffer_size_ptr)) *p = kXContentDataSize * items_per_enumerate;
  auto e = std::make_shared<Enumerator>(kXContentDataSize, items_per_enumerate);
  std::vector<std::string> names;
  {
    std::error_code ec;
    const fs::path root = PackagePath("");
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code ec2;
      if (!it->is_directory(ec2)) continue;
      const std::u8string u8 = it->path().filename().u8string();
      std::string name(u8.begin(), u8.end());
      if (name.empty() || name.size() > kContentFileNameSize || !io::IsValidComponent(name)) continue;
      names.push_back(std::move(name));
    }
  }
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    uint8_t item[kXContentDataSize] = {};
    *reinterpret_cast<be<uint32_t>*>(item) = kHddDeviceId;
    *reinterpret_cast<be<uint32_t>*>(item + 4) = content_type;
    for (size_t i = 0; i < name.size() && i < 127; ++i)
      *reinterpret_cast<be<uint16_t>*>(item + 8 + 2 * i) = uint16_t(uint8_t(name[i]));
    std::memcpy(item + kContentFileNameOffset, name.data(), name.size());
    e->Append(item);
  }
  *P32(handle_out) = HandleCreate(std::move(e));
  return kErrorSuccess;
}

// Xenia: XamContentAggregateCreateEnumerator_entry (xuid, device_id,
// content_type, unknown, handle_out): one XCONTENT_AGGREGATE_DATA (XCONTENT_DATA
// + xuid + title id) per enumeration. Deviation: no title has installed
// downloadable content here, so the enumerator is empty.
uint32_t XamContentAggregateCreateEnumeratorImpl(uint64_t /*xuid*/, uint32_t /*device_id*/,
                                                 uint32_t /*content_type*/, uint32_t /*unknown*/,
                                                 uint32_t handle_out) {
  if (!handle_out) return kEInvalidArg;
  *P32(handle_out) = HandleCreate(std::make_shared<Enumerator>(
      kXContentDataSize + 12, 1, Enumerator::Messages{kXamAppId, kMsgContentAggregateEnumerate, 0x20010, 0xFF}));
  return kErrorSuccess;
}

// Xenia: XamGetPrivateEnumStructureFromHandle_entry: the enumerator's guest
// X_KENUMERATOR, with a reference the caller drops with ObDereferenceObject.
uint32_t XamGetPrivateEnumStructureFromHandleImpl(uint32_t handle, be<uint32_t>* out_object) {
  auto e = std::dynamic_pointer_cast<Enumerator>(HandleGet(handle));
  if (!e) return kStatusInvalidHandle;
  const uint32_t object = e->GuestPrivate(handle);
  if (!object) return kStatusNoMemory;
  ObjectReference(object, e);
  if (out_object) *out_object = object;
  return 0;
}

// Xenia: xeXamEnumerate / XamEnumerate_entry.
uint32_t XamEnumerateImpl(uint32_t handle, uint32_t /*flags*/, uint32_t buffer, uint32_t /*buffer_length*/,
                          uint32_t items_returned, uint32_t overlapped) {
  auto e = std::dynamic_pointer_cast<Enumerator>(HandleGet(handle));
  if (!e) return kErrorInvalidHandle;
  uint32_t count = 0;
  const auto run = [&]() -> uint32_t {
    if (!buffer) return kErrorInvalidParameter;
    return e->WriteItems(GuestPtr<uint8_t>(buffer), &count);
  };
  if (!overlapped && items_returned) {
    const uint32_t result = run();
    *P32(items_returned) = result == kErrorSuccess ? count : 0;
    return result;
  }
  if (overlapped) {
    const uint32_t result = run();
    return CompleteImmediateEx(overlapped, result, HResultFromWin32(result), count);
  }
  return kErrorInvalidParameter;
}

// Xenia: XamCreateEnumeratorHandle_entry (stub).
uint32_t XamCreateEnumeratorHandleImpl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                       uint32_t) {
  return kErrorInvalidParameter;
}

// ---- user ---------------------------------------------------------------------------

// Xenia: XamUserGetSigninState_entry.
uint32_t XamUserGetSigninStateImpl(uint32_t user_index) { return user_index == 0 ? kSigninLocal : 0; }

// Device context of a user's controller. Always zeroes the output, so a caller that
// ignores the result never reads stale stack data; succeeds for the signed-in user 0
// and the "any user" index 0xFF, the other users have no pad (device not connected).
uint32_t XamUserGetDeviceContextImpl(uint32_t user_index, uint32_t /*unk*/, uint32_t out_ptr) {
  if (be<uint32_t>* out = GuestPtr<be<uint32_t>>(out_ptr)) *out = 0;
  return user_index == 0 || (user_index & 0xFF) == 0xFF ? 0 : kErrorDeviceNotConnected;
}

// Xenia: XamUserGetXUID_entry. The local profile is of type 1|2.
uint32_t XamUserGetXUIDImpl(uint32_t user_index, uint32_t type_mask, uint32_t xuid_ptr) {
  be<uint64_t>* out = GuestPtr<be<uint64_t>>(xuid_ptr);
  if (!out) return kEInvalidArg;
  uint32_t result = kENoSuchUser;
  uint64_t xuid = 0;
  if (user_index < kUserCount) {
    if (user_index == 0 && ((1 | 2) & type_mask) & (1 | 2 | 4)) {
      xuid = kLocalXuid;
      result = 0;
    }
  } else {
    result = kEInvalidArg;
  }
  *out = xuid;
  return result;
}

// Xenia: XamUserGetSigninInfo_entry. X_USER_SIGNIN_INFO (40 bytes): xuid u64,
// unk08, signin_state, unk10, unk14, name[16].
uint32_t XamUserGetSigninInfoImpl(uint32_t user_index, uint32_t /*flags*/, uint32_t info) {
  uint8_t* p = GuestPtr<uint8_t>(info);
  if (!p) return kEInvalidArg;
  std::memset(p, 0, 40);
  if (user_index) return kENoSuchUser;
  *reinterpret_cast<be<uint64_t>*>(p) = kLocalXuid;
  *reinterpret_cast<be<uint32_t>*>(p + 12) = kSigninLocal;
  std::memcpy(p + 24, kUserName, sizeof(kUserName));
  return 0;
}

// Xenia: XamUserGetName_entry (at most 16 bytes, terminator included).
uint32_t XamUserGetNameImpl(uint32_t user_index, uint32_t buffer, uint32_t buffer_len) {
  if (user_index >= kUserCount) return kEInvalidArg;
  if (user_index) return kENoSuchUser;
  char* out = GuestPtr<char>(buffer);
  const uint32_t n = std::min<uint32_t>(buffer_len, 16);
  if (out && n) {
    std::memset(out, 0, n);
    std::memcpy(out, kUserName, std::min<size_t>(sizeof(kUserName) - 1, n - 1));
  }
  return 0;
}

// Xenia: XamUserCheckPrivilege_entry: denies everything ("if we deny everything,
// games do not try to go online").
uint32_t XamUserCheckPrivilegeImpl(uint32_t user_index, uint32_t /*mask*/, uint32_t out_value) {
  if (user_index != 0xFF) {
    if (user_index >= kUserCount) return kErrorInvalidParameter;
    if (user_index) return kErrorNoSuchUser;
  }
  if (be<uint32_t>* p = P32(out_value)) *p = 0;
  return kErrorSuccess;
}

// Xenia: XamUserAreUsersFriends_entry: no friends.
uint32_t XamUserAreUsersFriendsImpl(uint32_t user_index, uint32_t, uint32_t, uint32_t out_value, uint32_t overlapped) {
  uint32_t result;
  if (user_index >= kUserCount)
    result = kErrorInvalidParameter;
  else if (user_index == 0)
    result = kErrorSuccess;
  else
    result = kErrorNoSuchUser;
  if (out_value) {
    *P32(out_value) = 0;
    return result;
  }
  if (overlapped)
    return CompleteImmediateEx(overlapped, result == kErrorSuccess ? kErrorSuccess : kErrorFunctionFailed,
                               HResultFromWin32(result), 0);
  return kErrorInvalidParameter;
}

// Xenia: XamUserReadProfileSettings_entry / XamUserReadProfileSettingsEx.
// X_USER_READ_PROFILE_SETTINGS (count, settings_ptr) followed by
// X_USER_PROFILE_SETTING (40 bytes: from, unk04, user_index|xuid, setting_id,
// unk14, data[16]). Deviation: the profile has no saved settings;
// each comes back "not set" (from 0, data zeroed) with the space for
// strings and binaries computed from the id as in Xenia, so the game uses its
// default values instead of garbage data.
uint32_t XamUserReadProfileSettingsImpl(uint32_t /*title_id*/, uint32_t user_index, uint32_t xuid_count,
                                        uint32_t xuids, uint32_t setting_count, uint32_t setting_ids,
                                        uint32_t buffer_size_ptr, uint32_t buffer, uint32_t overlapped) {
  constexpr uint32_t kSettingSize = 40, kHeaderSize = 8;
  if (!xuid_count)
    xuids = 0;
  else
    xuid_count = 1;
  if (setting_count < 1 || setting_count > 32 || !setting_ids || !buffer_size_ptr) return kErrorInvalidParameter;
  const uint32_t buffer_size = *P32(buffer_size_ptr);
  if (buffer_size && !buffer) return kErrorInvalidParameter;

  uint32_t header = kHeaderSize, data = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    header += kSettingSize;
    const uint32_t key = *P32(setting_ids + 4 * i);
    const uint32_t type = key >> 28, size = (key >> 16) & 0xFFF;
    if (type == 4 /*WSTRING*/ || type == 6 /*BINARY*/) data += size;
  }
  const uint32_t needed = header + data;
  if (buffer && !GuestRangeValid(buffer, needed)) return kErrorInvalidParameter;
  if (!buffer || buffer_size < needed) {
    if (!buffer_size) *P32(buffer_size_ptr) = needed;
    return kErrorInsufficientBuffer;
  }
  if (!xuids && user_index) {
    if (overlapped) return CompleteImmediate(overlapped, kErrorNoSuchUser);
    return kErrorNoSuchUser;
  }
  uint8_t* out = GuestPtr<uint8_t>(buffer);
  std::memset(out, 0, needed);
  *reinterpret_cast<be<uint32_t>*>(out) = setting_count;
  *reinterpret_cast<be<uint32_t>*>(out + 4) = buffer + kHeaderSize;
  for (uint32_t i = 0; i < setting_count; ++i) {
    uint8_t* s = out + kHeaderSize + i * kSettingSize;
    if (xuids)
      *reinterpret_cast<be<uint64_t>*>(s + 8) = kLocalXuid;
    else
      *reinterpret_cast<be<uint32_t>*>(s + 8) = user_index;
    *reinterpret_cast<be<uint32_t>*>(s + 16) = *P32(setting_ids + 4 * i);
  }
  if (overlapped) return CompleteImmediate(overlapped, kErrorSuccess);
  return kErrorSuccess;
}

// Xenia: XamUserWriteProfileSettings_entry. Deviation: nothing is kept
// (the profile is not persistent); the result is success.
uint32_t XamUserWriteProfileSettingsImpl(uint32_t /*title_id*/, uint32_t user_index, uint32_t setting_count,
                                         uint32_t settings, uint32_t overlapped) {
  uint32_t result = kErrorSuccess;
  if (!setting_count || !settings)
    return kErrorInvalidParameter;
  if (user_index) result = kErrorNoSuchUser;
  if (overlapped) return CompleteImmediate(overlapped, result);
  return result;
}

// Xenia: XamWriteGamerTile_entry (stub).
uint32_t XamWriteGamerTileImpl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t overlapped) {
  if (overlapped) return CompleteImmediate(overlapped, kErrorSuccess);
  return kErrorSuccess;
}

// Xenia: XamUserCreateAchievementEnumerator_entry. Deviation: without XDBF the
// title has no achievements, so the enumerator is empty (NO_MORE_FILES).
uint32_t XamUserCreateAchievementEnumeratorImpl(uint32_t /*title_id*/, uint32_t user_index, uint32_t /*xuid*/,
                                                uint32_t flags, uint32_t /*offset*/, uint32_t count,
                                                uint32_t buffer_size_ptr, uint32_t handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr || user_index >= kUserCount) return kErrorInvalidParameter;
  uint32_t entry_size = 36;  // X_ACHIEVEMENT_DETAILS
  if (flags & 7) entry_size += 464;  // string buffer
  *P32(buffer_size_ptr) = entry_size * count;
  *P32(handle_ptr) = HandleCreate(std::make_shared<Enumerator>(entry_size, count));
  return kErrorSuccess;
}

// ---- system UI -----------------------------------------------------------------------

// Xenia: xeXamDispatchHeadless: XN_SYS_UI (on/off) around the UI.
// Deviation: the switch-off is immediate, without the 100 ms delay.
template <class F>
uint32_t DispatchHeadless(F&& run, uint32_t overlapped) {
  BroadcastNotification(kXnSysUi, 1);
  const uint32_t result = run();
  uint32_t ret = result;
  if (overlapped) ret = CompleteImmediate(overlapped, result);
  BroadcastNotification(kXnSysUi, 0);
  return ret;
}

// Xenia: XamShowDeviceSelectorUI_entry: always picks device 1.
uint32_t XamShowDeviceSelectorUIImpl(uint32_t /*user_index*/, uint32_t /*content_type*/, uint32_t /*content_flags*/,
                                     uint64_t /*total_requested*/, uint32_t device_id_ptr, uint32_t overlapped) {
  return DispatchHeadless(
      [&] {
        if (be<uint32_t>* p = P32(device_id_ptr)) *p = kHddDeviceId;
        return kErrorSuccess;
      },
      overlapped);
}

// Xenia: XamShowKeyboardUI_entry in headless mode: the default text
// comes back in the buffer (UTF-16 BE, at most buffer_length - 1 characters).
uint32_t XamShowKeyboardUIImpl(uint32_t /*user_index*/, uint32_t /*flags*/, uint32_t default_text, uint32_t /*title*/,
                               uint32_t /*description*/, uint32_t buffer, uint32_t buffer_length,
                               uint32_t overlapped) {
  be<uint16_t>* out = GuestPtr<be<uint16_t>>(buffer);
  if (!out || !GuestRangeValid(buffer, uint64_t(buffer_length) * 2)) return kErrorInvalidParameter;
  return DispatchHeadless(
      [&] {
        if (buffer_length) std::memset(out, 0, size_t(buffer_length) * 2);
        if (const be<uint16_t>* in = GuestPtr<be<uint16_t>>(default_text)) {
          for (uint32_t i = 0; i + 1 < buffer_length && in[i] != 0; ++i) out[i] = uint16_t(in[i]);
        }
        return kErrorSuccess;
      },
      overlapped);
}

// XamShowMessageBoxUIEx: not in Xenia (only in xam_table.inc). Signature of
// Xenia Canary (user, title, text, button count, buttons, active button,
// flags, unused, result*, overlapped); like Xenia's headless XamShowMessageBoxUI
// it picks the active button.
uint32_t XamShowMessageBoxUIExImpl(uint32_t /*user_index*/, uint32_t /*title*/, uint32_t /*text*/,
                                   uint32_t /*button_count*/, uint32_t /*buttons*/, uint32_t active_button,
                                   uint32_t /*flags*/, uint32_t /*unused*/, uint32_t result_ptr,
                                   uint32_t overlapped) {
  return DispatchHeadless(
      [&] {
        if (be<uint32_t>* p = P32(result_ptr)) *p = active_button;
        return kErrorSuccess;
      },
      overlapped);
}

// Xenia: XamShowSigninUI_entry: notifies the sign-in change and the UI switch-off
// ("games wait for this notification").
uint32_t XamShowSigninUIImpl(uint32_t /*unk*/, uint32_t /*unk_mask*/) {
  BroadcastNotification(kXnSysSigninChanged, 1);
  BroadcastNotification(kXnSysUi, 0);
  return kErrorSuccess;
}

// ---- notifications -------------------------------------------------------------------------

// Xenia: xeXamNotifyCreateListener + KernelState::RegisterNotifyListener.
// Deviation (project decision): the initial notifications the game
// expects, XN_SYS_SIGNINCHANGED and XN_SYS_STORAGEDEVICESCHANGED (Xenia sends
// XN_SYS_UI, SIGNINCHANGED x2 and input only to the first listener), go to every
// newly created listener whose mask includes the system area.
uint32_t XamNotifyCreateListenerImpl(uint64_t mask, uint32_t max_version) {
  if (max_version > 10) max_version = 10;
  auto listener = std::make_shared<NotifyListener>(mask, max_version);
  if (listener->mask() & kXnSystemArea) {
    listener->Enqueue(kXnSysSigninChanged, 1);  // bit 0: user 0 authenticated
    listener->Enqueue(kXnSysStorageDevicesChanged, 0);
  }
  {
    std::lock_guard lock(g_listeners_mutex);
    std::erase_if(g_listeners, [](const std::weak_ptr<NotifyListener>& w) { return w.expired(); });
    g_listeners.push_back(listener);
  }
  return HandleCreate(std::move(listener));
}

// Xenia: XNotifyGetNext_entry. With match_id it looks for that notification; param_ptr
// may be null.
uint32_t XNotifyGetNextImpl(uint32_t handle, uint32_t match_id, uint32_t id_ptr, uint32_t param_ptr) {
  if (be<uint32_t>* p = P32(param_ptr)) *p = 0;
  be<uint32_t>* id_out = P32(id_ptr);
  if (!id_out) return 0;
  *id_out = 0;
  auto listener = std::dynamic_pointer_cast<NotifyListener>(HandleGet(handle));
  if (!listener) return 0;
  uint32_t id = 0, param = 0;
  const bool dequeued = listener->Dequeue(match_id, &id, &param);
  if (dequeued) {
    *id_out = id;
    if (be<uint32_t>* p = P32(param_ptr)) *p = param;
  }
  return dequeued ? 1 : 0;
}

// Xenia: XNotifyPositionUI_entry (ignored).
void XNotifyPositionUIImpl(uint32_t) {}

// ---- system information ---------------------------------------------------------------

// Xenia: XGetLanguage_entry, with the language from --language (default English).
uint32_t XGetLanguageImpl() { return g_language.load(); }

// Xenia: XGetGameRegion_entry.
uint32_t XGetGameRegionImpl() { return 0xFFFF; }

// Xenia: XamGetSystemVersion_entry (stub, 0).
uint32_t XamGetSystemVersionImpl() { return 0; }

// Xenia: XGetVideoMode_entry -> VdQueryVideoMode (xboxkrnl_video.cc):
// 1280x720 progressive, widescreen, HD, 60 Hz, NTSC. X_VIDEO_MODE (48 bytes).
void XGetVideoModeImpl(uint32_t video_mode) {
  uint8_t* p = GuestPtr<uint8_t>(video_mode);
  if (!p) return;
  std::memset(p, 0, 48);
  *reinterpret_cast<be<uint32_t>*>(p + 0) = 1280;
  *reinterpret_cast<be<uint32_t>*>(p + 4) = 720;
  *reinterpret_cast<be<uint32_t>*>(p + 8) = 0;   // interlaced
  *reinterpret_cast<be<uint32_t>*>(p + 12) = 1;  // widescreen
  *reinterpret_cast<be<uint32_t>*>(p + 16) = 1;  // hi-def
  *reinterpret_cast<be<float>*>(p + 20) = 60.0f;
  *reinterpret_cast<be<uint32_t>*>(p + 24) = 1;     // video_standard: NTSC
  *reinterpret_cast<be<uint32_t>*>(p + 28) = 0x4A;  // unknown_0x8a
  *reinterpret_cast<be<uint32_t>*>(p + 32) = 0x01;  // unknown_0x01
}

// Xenia: XamLoaderGetLaunchDataSize_entry / XamLoaderGetLaunchData_entry:
// no launch data (the title starts from the menu, not from another title).
uint32_t XamLoaderGetLaunchDataSizeImpl(uint32_t size_ptr) {
  if (!size_ptr) return kErrorInvalidParameter;
  *P32(size_ptr) = 0;
  return kErrorNotFound;
}
uint32_t XamLoaderGetLaunchDataImpl(uint32_t /*buffer*/, uint32_t /*size*/) { return kErrorNotFound; }

// Xenia: XamAlloc_entry / XamFree_entry (system heap -> process heap).
uint32_t XamAllocImpl(uint32_t /*unk*/, uint32_t size, uint32_t out_ptr) {
  MemorySystem* m = Memory();
  be<uint32_t>* out = P32(out_ptr);
  if (!out) return kErrorInvalidParameter;
  *out = m ? m->heap.Alloc(size) : 0;
  return kErrorSuccess;
}
uint32_t XamFreeImpl(uint32_t ptr) {
  if (MemorySystem* m = Memory(); m && ptr) m->heap.Free(ptr);
  return kErrorSuccess;
}

// ---- messages to system apps (XMsg*) ------------------------------------------

// Xenia: AppManager::DispatchMessageSync/Async (xam/app_manager.cc) with the
// apps in xam/apps/. Handled: XLiveBaseApp (0xFC, xlivebase_app.cc, offline),
// XmpApp (0xFA, xmp_app.cc) and XgiApp (0xFB, xgi_app.cc) without real sessions or
// playlists; XamApp (0xFE) answers X_E_FAIL (logged). An unhandled
// message of a known app also answers X_E_FAIL (logged), like Xenia; an
// unknown app X_E_NOTFOUND.
constexpr uint32_t kESuccess = 0;
constexpr uint32_t kEFail = 0x80004005;
constexpr uint32_t kENotFound = 0x80070490;

// Minimal state of XmpApp (xmp_app.cc): no playlist, so the state stays
// kIdle (0) and next/previous/info find nothing. Xenia's initial values:
// playback client kTitle (1), mode 0, repeat 0, volume 1.0f.
constexpr uint32_t kXmpNotifyBehaviorChanged = 0x0A000002;    // kMsgPlaybackBehaviorChanged
constexpr uint32_t kXmpNotifyControllerChanged = 0x0A000003;  // kMsgPlaybackControllerChanged
std::atomic<uint32_t> g_xmp_playback_client{1};
std::atomic<uint32_t> g_xmp_playback_mode{0};
std::atomic<uint32_t> g_xmp_repeat_mode{0};
std::atomic<uint32_t> g_xmp_flags{0};
std::atomic<uint32_t> g_xmp_volume_bits{0x3F800000};  // 1.0f

// Xenia: XmpApp::DispatchMessageSync (xmp_app.cc). nullopt = not handled.
// Title playlists (0x00070002/0D/13) and track info (0x0000E) require Xenia's
// playlists: they stay unhandled (X_E_FAIL).
std::optional<uint32_t> DispatchXmpMessage(uint32_t message, uint32_t buffer) {
  be<uint32_t>* args = P32(buffer);
  if (!args) return std::nullopt;
  switch (message) {
    case 0x00070003:  // XMPContinue: without a playlist the state stays idle
    case 0x00070004:  // XMPStop
    case 0x00070005:  // XMPPause
      return kESuccess;
    case 0x00070006:  // XMPNext
    case 0x00070007:  // XMPPrevious: no active playlist
      return kENotFound;
    case 0x00070008:  // XMPSetPlaybackBehavior {client, mode, repeat, flags}
      g_xmp_playback_mode = args[1];
      g_xmp_repeat_mode = args[2];
      g_xmp_flags = args[3];
      BroadcastNotification(kXmpNotifyBehaviorChanged, 0);
      return kESuccess;
    case 0x00070009:  // XMPGetStatus {client, state_ptr}: kIdle
      if (be<uint32_t>* state = P32(args[1])) *state = 0;
      return kESuccess;
    case 0x0007000B:  // XMPGetVolume {client, volume_ptr}
      if (be<uint32_t>* volume = P32(args[1])) *volume = g_xmp_volume_bits.load();
      return kESuccess;
    case 0x0007000C:  // XMPSetVolume {client, float}
      g_xmp_volume_bits = static_cast<uint32_t>(args[1]);
      return kESuccess;
    case 0x0007001A:  // XMPSetPlaybackController {client, controller, playback_client}
      g_xmp_playback_client = args[2];
      BroadcastNotification(kXmpNotifyControllerChanged, args[2] == 0 ? 1 : 0);
      return kESuccess;
    case 0x0007001B:  // XMPGetPlaybackController {client, controller_ptr, locked_ptr}
      // controller 0 = the title controls the music, not locked.
      if (be<uint32_t>* controller = P32(args[1])) *controller = 0;
      if (be<uint32_t>* locked = P32(args[2])) *locked = 0;
      return kESuccess;
    case 0x00070029:  // XMPGetPlaybackBehavior {client, mode_ptr, repeat_ptr, flags_ptr}
      if (be<uint32_t>* mode = P32(args[1])) *mode = g_xmp_playback_mode.load();
      if (be<uint32_t>* repeat = P32(args[2])) *repeat = g_xmp_repeat_mode.load();
      if (be<uint32_t>* flags = P32(args[3])) *flags = g_xmp_flags.load();
      return kESuccess;
    case 0x0007002E:  // storage size for XamAlloc: 4 + tracks * 128
      if (be<uint32_t>* size = P32(args[2])) *size = 4 + static_cast<uint32_t>(args[1]) * 128;
      return kESuccess;
    default: return std::nullopt;
  }
}

// Xenia: XgiApp::DispatchMessageSync (xgi_app.cc). No sessions or
// scores: the messages Xenia only logs are answered as it does.
std::optional<uint32_t> DispatchXgiMessage(uint32_t message, uint32_t buffer) {
  switch (message) {
    case 0x000B0006:  // XGIUserSetContextEx
    case 0x000B0007:  // XGIUserSetPropertyEx
    case 0x000B0008:  // XGIUserWriteAchievements
    case 0x000B0010:  // XGISessionCreateImpl (Xenia only logs: no kernel object)
    case 0x000B0011:  // XGISessionDelete
    case 0x000B0012:  // XGISessionJoinLocal
    case 0x000B0014:  // XGI_unknown (score table)
    case 0x000B0015:  // XGI_unknown (score submission)
    case 0x000B0071:  // not implemented in Xenia, success
      return kESuccess;
    case 0x000B0041: {  // XGIUserGetContext: value 0 in the context, but X_E_FAIL
      be<uint32_t>* args = P32(buffer);
      if (!args) return kEFail;
      if (be<uint32_t>* context = P32(args[4])) context[1] = 0;
      return kEFail;
    }
    default: return std::nullopt;
  }
}
uint32_t DispatchAppMessage(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length) {
  if (app == 0xFC) {
    switch (message) {
      case 0x00058004:  // XLiveBaseGetLogonId: a boolean in the buffer
      case 0x00058006:  // XLiveBaseGetNatType: XONLINE_NAT_OPEN
        if (be<uint32_t>* p = P32(buffer)) *p = 1;
        return kESuccess;
      case 0x00058007: return 0x80151802;  // GetServiceInfo: ERROR_CONNECTION_INVALID
      case 0x00058046: return kESuccess;
      default: break;
    }
  } else if (app == 0xFA) {
    if (std::optional<uint32_t> r = DispatchXmpMessage(message, buffer)) return *r;
  } else if (app == 0xFB) {
    if (std::optional<uint32_t> r = DispatchXgiMessage(message, buffer)) return *r;
  } else if (app == kXamAppId && message == kMsgContentAggregateEnumerate) {
    // Xenia XamApp::DispatchMessageSync 0x0002000E: { user_index, ?, extra (the
    // X_KENUMERATOR's aggregate data), buffer, buffer_size, ?, length_ptr, ? };
    // the next item, X_E_NO_MORE_FILES when there are none.
    be<uint32_t>* args = P32(buffer);
    be<uint32_t>* extra = args ? P32(args[2]) : nullptr;
    auto e = extra ? std::dynamic_pointer_cast<Enumerator>(HandleGet(extra[1])) : nullptr;
    if (!e) return kEInvalidArg;
    uint32_t count = 0;
    const uint32_t result = args[3] ? e->WriteItems(GuestPtr<uint8_t>(args[3]), &count) : kErrorInvalidParameter;
    if (be<uint32_t>* length = P32(args[6])) *length = result == kErrorSuccess ? count : 0;
    return HResultFromWin32(result);
  } else if (app != kXamAppId) {
    LogLine("[kernel] XMsg: unknown app " + Hex(app) + " (message " + Hex(message) + ")");
    return kENotFound;
  }
  LogLine("[kernel] XMsg: message " + Hex(message) + " to app " + Hex(app) + " not handled (buffer " +
          Hex(buffer) + ", " + Hex(length) + "): X_E_FAIL");
  return kEFail;
}

// Xenia: XMsgInProcessCall_entry.
uint32_t XMsgInProcessCallImpl(uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2) {
  return DispatchAppMessage(app, message, arg1, arg2);
}

// Xenia: xeXMsgStartIORequestEx (XMsgStartIORequest and ...Ex): immediate result
// in the overlapped and ERROR_IO_PENDING; unknown app -> X_E_INVALIDARG.
uint32_t XMsgStartIORequestImpl(uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer,
                                uint32_t length) {
  uint32_t result = DispatchAppMessage(app, message, buffer, length);
  if (result == kENotFound) result = kEInvalidArg;
  if (overlapped) result = CompleteImmediate(overlapped, result);
  return result;
}
uint32_t XMsgStartIORequestExImpl(uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer,
                                  uint32_t length, uint32_t /*unknown*/) {
  return XMsgStartIORequestImpl(app, message, overlapped, buffer, length);
}

// Xenia: XMsgCancelIORequest_entry: requests are already completed; with
// wait != 0 the event is tried without blocking (Xenia Wait with timeout 0).
uint32_t XMsgCancelIORequestImpl(uint32_t /*overlapped*/, uint32_t /*wait*/) { return 0; }

// Xenia: XMsgCompleteIORequest_entry (CompleteOverlappedImmediateEx).
uint32_t XMsgCompleteIORequestImpl(uint32_t overlapped, uint32_t result, uint32_t extended_error,
                                   uint32_t length) {
  CompleteOverlappedEx(overlapped, result, extended_error, length);
  return kErrorSuccess;
}

// Xenia: XamGetOverlappedResult_entry: the result if already complete,
// otherwise ERROR_IO_INCOMPLETE without an event, or an infinite wait on it
// (Xenia waits whatever `wait` says).
uint32_t XamGetOverlappedResultImpl(uint32_t overlapped, be<uint32_t>* length, uint32_t /*wait*/) {
  be<uint32_t>* ov = P32(overlapped);
  if (!ov) return kErrorInvalidParameter;
  uint32_t result = ov[0];
  if (result == kErrorIoPending) {
    auto ev = ov[3] ? HandleGet(ov[3]) : nullptr;
    if (!ev) return kErrorIoIncomplete;
    const std::shared_ptr<KernelObject> objects[] = {ev};
    result = WaitAny(objects, nullptr, false) == 0 ? uint32_t(ov[0]) : kErrorFunctionFailed;
  }
  if (length) *length = ov[1];
  return result;
}

// Xenia: XamTaskSchedule_entry: the callback runs with the message on a new
// thread whose handle goes to handle_ptr.
uint32_t XamTaskScheduleImpl(uint32_t callback, uint32_t message, uint32_t /*unknown*/, be<uint32_t>* handle_ptr) {
  auto t = GuestThread::Create(callback, message, 0, false);
  if (!t) return kStatusNoMemory;
  const uint32_t handle = HandleCreate(std::move(t));
  if (handle_ptr) *handle_ptr = handle;
  return 0;
}

// Xenia: XamTaskCloseHandle_entry: releases the task's handle. A task still
// running keeps its thread alive until it ends (GuestThread holds itself).
uint32_t XamTaskCloseHandleImpl(uint32_t handle) {
  return HandleClose(handle) ? kErrorSuccess : kStatusInvalidHandle;
}

}  // namespace

// ---- public API (xam.h) ---------------------------------------------------------------------

std::optional<uint32_t> LanguageFromCode(std::string_view code) {
  const std::string c = LowerAscii(std::string(code));
  static const std::pair<const char*, uint32_t> kCodes[] = {
      {"en", 1}, {"ja", 2}, {"de", 3}, {"fr", 4}, {"es", 5}, {"it", 6}, {"ko", 7},
      {"zh", 8}, {"zh-tw", 8}, {"pt", 9}, {"zh-cn", 10}, {"pl", 11}, {"ru", 12}};
  for (const auto& [name, value] : kCodes)
    if (c == name) return value;
  return std::nullopt;
}

void SetLanguage(uint32_t language) { g_language = language; }
uint32_t Language() { return g_language.load(); }

void SetContentRoot(const fs::path& root) {
  std::lock_guard lock(g_content_mutex);
  g_content_root = root;
}

void BroadcastNotification(uint32_t id, uint32_t data) {
  std::vector<std::shared_ptr<NotifyListener>> live;
  {
    std::lock_guard lock(g_listeners_mutex);
    for (auto& w : g_listeners)
      if (auto l = w.lock()) live.push_back(std::move(l));
  }
  for (auto& l : live) l->Enqueue(id, data);
}

void Reset() {
  std::map<std::string, OpenPackage> open;
  {
    std::lock_guard lock(g_content_mutex);
    open.swap(g_open);
  }
  for (const auto& [root, pkg] : open) io::RemoveMount(pkg.device);
  {
    std::lock_guard lock(g_listeners_mutex);
    g_listeners.clear();
  }
  g_language = 1;
}

}  // namespace kernel::xam

KERNEL_EXPORT(XamUserGetSigninState, kernel::xam::XamUserGetSigninStateImpl)
KERNEL_EXPORT(XamUserGetDeviceContext, kernel::xam::XamUserGetDeviceContextImpl)
KERNEL_EXPORT(XamUserGetXUID, kernel::xam::XamUserGetXUIDImpl)
KERNEL_EXPORT(XamUserGetSigninInfo, kernel::xam::XamUserGetSigninInfoImpl)
KERNEL_EXPORT(XamUserGetName, kernel::xam::XamUserGetNameImpl)
KERNEL_EXPORT(XamUserCheckPrivilege, kernel::xam::XamUserCheckPrivilegeImpl)
KERNEL_EXPORT(XamUserAreUsersFriends, kernel::xam::XamUserAreUsersFriendsImpl)
KERNEL_EXPORT(XamUserReadProfileSettings, kernel::xam::XamUserReadProfileSettingsImpl)
KERNEL_EXPORT(XamUserWriteProfileSettings, kernel::xam::XamUserWriteProfileSettingsImpl)
KERNEL_EXPORT(XamWriteGamerTile, kernel::xam::XamWriteGamerTileImpl)
KERNEL_EXPORT(XamUserCreateAchievementEnumerator, kernel::xam::XamUserCreateAchievementEnumeratorImpl)
KERNEL_EXPORT(XamContentCreateEx, kernel::xam::XamContentCreateExImpl)
KERNEL_EXPORT(XamContentFlush, kernel::xam::XamContentFlushImpl)
KERNEL_EXPORT(XamContentClose, kernel::xam::XamContentCloseImpl)
KERNEL_EXPORT(XamContentGetCreator, kernel::xam::XamContentGetCreatorImpl)
KERNEL_EXPORT(XamContentDelete, kernel::xam::XamContentDeleteImpl)
KERNEL_EXPORT(XamContentSetThumbnail, kernel::xam::XamContentSetThumbnailImpl)
KERNEL_EXPORT(XamContentGetDeviceState, kernel::xam::XamContentGetDeviceStateImpl)
KERNEL_EXPORT(XamContentGetDeviceData, kernel::xam::XamContentGetDeviceDataImpl)
KERNEL_EXPORT(XamContentCreateEnumerator, kernel::xam::XamContentCreateEnumeratorImpl)
KERNEL_EXPORT(XamContentAggregateCreateEnumerator, kernel::xam::XamContentAggregateCreateEnumeratorImpl)
KERNEL_EXPORT(XamEnumerate, kernel::xam::XamEnumerateImpl)
KERNEL_EXPORT(XamCreateEnumeratorHandle, kernel::xam::XamCreateEnumeratorHandleImpl)
KERNEL_EXPORT(XamShowSigninUI, kernel::xam::XamShowSigninUIImpl)
KERNEL_EXPORT(XamShowDeviceSelectorUI, kernel::xam::XamShowDeviceSelectorUIImpl)
KERNEL_EXPORT(XamShowKeyboardUI, kernel::xam::XamShowKeyboardUIImpl)
KERNEL_EXPORT(XamShowMessageBoxUIEx, kernel::xam::XamShowMessageBoxUIExImpl)
KERNEL_EXPORT(XamNotifyCreateListener, kernel::xam::XamNotifyCreateListenerImpl)
KERNEL_EXPORT(XNotifyGetNext, kernel::xam::XNotifyGetNextImpl)
KERNEL_EXPORT(XNotifyPositionUI, kernel::xam::XNotifyPositionUIImpl)
KERNEL_EXPORT(XGetLanguage, kernel::xam::XGetLanguageImpl)
KERNEL_EXPORT(XGetGameRegion, kernel::xam::XGetGameRegionImpl)
KERNEL_EXPORT(XamGetSystemVersion, kernel::xam::XamGetSystemVersionImpl)
KERNEL_EXPORT(XGetVideoMode, kernel::xam::XGetVideoModeImpl)
KERNEL_EXPORT(XamLoaderGetLaunchDataSize, kernel::xam::XamLoaderGetLaunchDataSizeImpl)
KERNEL_EXPORT(XamLoaderGetLaunchData, kernel::xam::XamLoaderGetLaunchDataImpl)
KERNEL_EXPORT(XamAlloc, kernel::xam::XamAllocImpl)
KERNEL_EXPORT(XamFree, kernel::xam::XamFreeImpl)
KERNEL_EXPORT(XMsgInProcessCall, kernel::xam::XMsgInProcessCallImpl)
KERNEL_EXPORT(XMsgStartIORequest, kernel::xam::XMsgStartIORequestImpl)
KERNEL_EXPORT(XMsgStartIORequestEx, kernel::xam::XMsgStartIORequestExImpl)
KERNEL_EXPORT(XMsgCancelIORequest, kernel::xam::XMsgCancelIORequestImpl)
KERNEL_EXPORT(XMsgCompleteIORequest, kernel::xam::XMsgCompleteIORequestImpl)
KERNEL_EXPORT(XamGetOverlappedResult, kernel::xam::XamGetOverlappedResultImpl)
KERNEL_EXPORT(XamTaskSchedule, kernel::xam::XamTaskScheduleImpl)
KERNEL_EXPORT(XamTaskCloseHandle, kernel::xam::XamTaskCloseHandleImpl)
KERNEL_EXPORT(XamGetPrivateEnumStructureFromHandle, kernel::xam::XamGetPrivateEnumStructureFromHandleImpl)
