// File system exports: NtCreateFile/NtOpenFile, reads and writes
// (synchronous, completed as async I/O that finishes immediately), file info
// query and set, directory listing, volume, symbolic links.
//
// Each function cites its Xenia counterpart
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc, xboxkrnl_io_info.cc,
// xboxkrnl_ob.cc, src/xenia/kernel/xfile.cc and
// src/xenia/vfs/virtual_file_system.cc (OpenFile). Code written from scratch;
// deviations are noted case by case.
//
// Files and directories are KernelObjects in the handle table (NtClose,
// ObReferenceObjectByHandle and the waits work as for the other
// objects). Like XFile, a file is waitable and always signaled: the I/O is
// already finished when the export returns.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "kernel/guest.h"
#include "kernel/io/vfs.h"
#include "kernel/objects.h"
#include "kernel/registry.h"
#include "kernel/stats.h"
#include "kernel/threads.h"
#include "log.h"
#include "physical_watch.h"

namespace fs = std::filesystem;

namespace {

using namespace kernel;
using namespace kernel::io;

// ---- console constants (Xenia xboxkrnl_io.cc CreateOptions,
// vfs/entry.h FileDisposition/FileAction/X_FILE_ATTRIBUTES,
// base/filesystem.h FileAccess, kernel/info/file.h) ----------------------------

constexpr uint32_t kFileDirectoryFile = 0x01;
constexpr uint32_t kFileSynchronousIoAlert = 0x10;
constexpr uint32_t kFileSynchronousIoNonalert = 0x20;
constexpr uint32_t kFileNonDirectoryFile = 0x40;

enum Disposition : uint32_t { kSupersede = 0, kOpen = 1, kCreate = 2, kOpenIf = 3, kOverwrite = 4, kOverwriteIf = 5 };
enum Action : uint32_t { kSuperseded = 0, kOpened = 1, kCreated = 2, kOverwritten = 3, kExists = 4, kDoesNotExist = 5 };

constexpr uint32_t kGenericRead = 0x80000000;
constexpr uint32_t kGenericWrite = 0x40000000;
constexpr uint32_t kGenericAll = 0x10000000;
constexpr uint32_t kFileReadData = 0x1;
constexpr uint32_t kFileWriteData = 0x2;
constexpr uint32_t kFileAppendData = 0x4;

constexpr uint32_t kAttrReadOnly = 0x01;
constexpr uint32_t kAttrDirectory = 0x10;
constexpr uint32_t kAttrNormal = 0x80;

constexpr uint32_t kObDosDevices = 0xFFFFFFFD;  // RootDirectory "\??\"
constexpr uint32_t kInvalidHandleValue = 0xFFFFFFFF;

// ByteOffset LARGE_INTEGER: FILE_WRITE_TO_END_OF_FILE (-1) and
// FILE_USE_FILE_POINTER_POSITION (LowPart 0xFFFFFFFE, HighPart -1).
constexpr uint64_t kWriteToEndOfFile = 0xFFFFFFFFFFFFFFFFull;
constexpr uint64_t kUseFilePointer = 0xFFFFFFFFFFFFFFFEull;

// X_FILE_INFORMATION_CLASS (Xenia kernel/info/file.h).
enum InfoClass : uint32_t {
  kFileDirectoryInformation = 1,
  kFileBasicInformation = 4,
  kFileStandardInformation = 5,
  kFileInternalInformation = 6,
  kFileNameInformation = 9,
  kFileRenameInformation = 10,
  kFileLinkInformation = 11,
  kFileDispositionInformation = 13,
  kFilePositionInformation = 14,
  kFileModeInformation = 16,
  kFileAlignmentInformation = 17,
  kFileAllocationInformation = 19,
  kFileEndOfFileInformation = 20,
  kFileMountPartitionInformation = 23,
  kFileMountPartitionsInformation = 24,
  kFileSectorInformation = 26,
  kFileXctdCompressionInformation = 27,
  kFileCompletionInformation = 30,
  kFileIoPriorityInformation = 32,
  kFileNetworkOpenInformation = 34,
};
// FS_INFORMATION_CLASS (Xenia kernel/info/volume.h).
enum FsInfoClass : uint32_t {
  kFileFsVolumeInformation = 1,
  kFileFsSizeInformation = 3,
  kFileFsDeviceInformation = 4,
  kFileFsAttributeInformation = 5,
};

// ---- guest structures (big-endian) ----------------------------------------------

#pragma pack(push, 1)
struct AnsiString {  // X_ANSI_STRING
  be<uint16_t> length;
  be<uint16_t> maximum_length;
  be<uint32_t> buffer;
};
struct ObjectAttributes {  // X_OBJECT_ATTRIBUTES
  be<uint32_t> root_directory;
  be<uint32_t> name;  // AnsiString*
  be<uint32_t> attributes;
};
struct IoStatusBlock {  // X_IO_STATUS_BLOCK
  be<uint32_t> status;
  be<uint32_t> information;
};
struct NetworkOpenInformation {  // X_FILE_NETWORK_OPEN_INFORMATION
  be<uint64_t> creation_time;
  be<uint64_t> last_access_time;
  be<uint64_t> last_write_time;
  be<uint64_t> change_time;
  be<uint64_t> allocation_size;
  be<uint64_t> end_of_file;
  be<uint32_t> attributes;
  be<uint32_t> pad;
};
struct BasicInformation {  // FILE_BASIC_INFORMATION
  be<uint64_t> creation_time;
  be<uint64_t> last_access_time;
  be<uint64_t> last_write_time;
  be<uint64_t> change_time;
  be<uint32_t> attributes;
  be<uint32_t> pad;
};
struct StandardInformation {  // FILE_STANDARD_INFORMATION
  be<uint64_t> allocation_size;
  be<uint64_t> end_of_file;
  be<uint32_t> number_of_links;
  uint8_t delete_pending;
  uint8_t directory;
  uint8_t pad[2];
};
struct DirectoryInformation {  // X_FILE_DIRECTORY_INFORMATION (Xenia xfile.h)
  be<uint32_t> next_entry_offset;
  be<uint32_t> file_index;
  be<uint64_t> creation_time;
  be<uint64_t> last_access_time;
  be<uint64_t> last_write_time;
  be<uint64_t> change_time;
  be<uint64_t> end_of_file;
  be<uint64_t> allocation_size;
  be<uint32_t> attributes;
  be<uint32_t> file_name_length;
  char file_name[1];
};
struct FsVolumeInformation {  // X_FILE_FS_VOLUME_INFORMATION
  be<uint64_t> creation_time;
  be<uint32_t> serial_number;
  be<uint32_t> label_length;
  uint8_t supports_objects;
  char label[1];
  uint8_t pad[6];
};
struct FsSizeInformation {  // X_FILE_FS_SIZE_INFORMATION
  be<uint64_t> total_allocation_units;
  be<uint64_t> available_allocation_units;
  be<uint32_t> sectors_per_allocation_unit;
  be<uint32_t> bytes_per_sector;
};
struct FsAttributeInformation {  // X_FILE_FS_ATTRIBUTE_INFORMATION
  be<uint32_t> attributes;
  be<int32_t> component_name_max_length;
  be<uint32_t> name_length;
  char name[1];
  uint8_t pad[3];
};
#pragma pack(pop)
static_assert(sizeof(NetworkOpenInformation) == 56);
static_assert(sizeof(BasicInformation) == 40);
static_assert(sizeof(StandardInformation) == 24);
static_assert(offsetof(DirectoryInformation, file_name) == 0x40);
static_assert(sizeof(FsVolumeInformation) == 24);
static_assert(sizeof(FsSizeInformation) == 24);
static_assert(sizeof(FsAttributeInformation) == 16);

// ---- log -------------------------------------------------------------------------

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", v);
  return buf;
}

// Guest errors and unsupported operations: at most kMaxLogged lines.
void LogIo(const std::string& what) {
  static std::atomic<int> logged{0};
  constexpr int kMaxLogged = 200;
  const int n = logged.fetch_add(1);
  if (n < kMaxLogged)
    LogLine("[io] " + what + " (caller " + Hex(ExportCaller()) + ")");
  else if (n == kMaxLogged)
    LogLine("[io] further file system messages not logged");
}

// ---- file objects -------------------------------------------------------------------

struct DirEntry {
  std::string name;
  uint64_t size = 0;
  uint64_t write_time = 0;
  bool directory = false;
};

// FILETIME (100 ns since 1601) of a std::filesystem instant.
uint64_t ToFileTime(fs::file_time_type t) {
  using Ticks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
  const int64_t ticks = std::chrono::duration_cast<Ticks>(sys.time_since_epoch()).count();
  return uint64_t(ticks + 116444736000000000LL);
}

uint64_t AllocationSize(uint64_t size) { return (size + 0xFFF) & ~uint64_t(0xFFF); }

class FileObject final : public KernelObject {
 public:
  FileObject(std::string guest, fs::path host, bool directory, bool mount_writable, bool can_read, bool can_write,
             bool synchronous, uint64_t min_size = 0)
      : guest_(std::move(guest)),
        host_(std::move(host)),
        min_size_(min_size),
        directory_(directory),
        mount_writable_(mount_writable),
        can_read_(can_read),
        can_write_(can_write),
        synchronous_(synchronous) {}

  // Like XFile: waitable, signaled (the I/O is always already finished).
  bool IsSignaled() const override { return true; }
  void Acquire() override {}
  uint8_t GuestType() const override { return kNotificationEvent; }

  const std::string& guest() const { return guest_; }
  const fs::path& host() const { return host_; }
  bool directory() const { return directory_; }
  bool mount_writable() const { return mount_writable_; }
  bool synchronous() const { return synchronous_; }
  uint64_t min_size() const { return min_size_; }

  bool OpenStream() {
    std::lock_guard g(mutex_);
    return OpenStreamLocked();
  }

  // Xenia XFile::Read / HostPathFile::ReadSync. offset: kUseFilePointer =
  // current position.
  uint32_t Read(uint8_t* dst, uint32_t length, uint64_t offset, uint32_t* done) {
    *done = 0;
    if (directory_) return kStatusInvalidParameter;
    if (!can_read_) return kStatusAccessDenied;
    std::lock_guard g(mutex_);
    if (offset == kUseFilePointer) offset = position_;
    if (length == 0) return kStatusSuccess;  // Xenia: length 0 succeeds
    if (!stream_.is_open()) return kStatusUnsuccessful;
    stream_.clear();
    stream_.seekg(std::streamoff(offset));
    if (stream_) stream_.read(reinterpret_cast<char*>(dst), std::streamsize(length));
    uint32_t n = stream_ ? length : uint32_t(std::max<std::streamsize>(stream_.gcount(), 0));
    stream_.clear();
    // Shorter replaced file (--skip-movie): zeros up to the original's size.
    if (n < length && offset + n < min_size_) {
      const uint32_t pad = uint32_t(std::min<uint64_t>(length - n, min_size_ - (offset + n)));
      std::memset(dst + n, 0, pad);
      n += pad;
    }
    if (n == 0) return kStatusEndOfFile;
    *done = n;
    position_ = offset + n;
    return kStatusSuccess;
  }

  // Xenia XFile::Write / HostPathFile::WriteSync. offset: kUseFilePointer =
  // current position, kWriteToEndOfFile = at the end.
  uint32_t Write(const uint8_t* src, uint32_t length, uint64_t offset, uint32_t* done) {
    *done = 0;
    if (directory_) return kStatusInvalidParameter;
    if (!mount_writable_ || !can_write_) return kStatusAccessDenied;
    std::lock_guard g(mutex_);
    if (!stream_.is_open()) return kStatusUnsuccessful;
    stream_.clear();
    if (offset == kUseFilePointer) offset = position_;
    if (offset == kWriteToEndOfFile) {
      stream_.seekp(0, std::ios::end);
      offset = uint64_t(std::streamoff(stream_.tellp()));
    } else {
      stream_.seekp(std::streamoff(offset));
    }
    if (length) stream_.write(reinterpret_cast<const char*>(src), std::streamsize(length));
    stream_.flush();
    if (!stream_) {
      stream_.clear();
      return kStatusUnsuccessful;
    }
    *done = length;
    position_ = offset + length;
    return kStatusSuccess;
  }

  uint64_t position() {
    std::lock_guard g(mutex_);
    return position_;
  }
  void set_position(uint64_t p) {
    std::lock_guard g(mutex_);
    position_ = p;
  }

  // Xenia HostPathFile::SetLength.
  uint32_t SetLength(uint64_t length) {
    if (directory_) return kStatusInvalidParameter;
    if (!mount_writable_ || !can_write_) return kStatusAccessDenied;
    std::lock_guard g(mutex_);
    stream_.close();  // no buffered data while the file changes underneath
    std::error_code ec;
    fs::resize_file(host_, length, ec);
    const bool reopened = OpenStreamLocked();
    return ec || !reopened ? kStatusUnsuccessful : kStatusSuccess;
  }

  void Flush() {
    std::lock_guard g(mutex_);
    if (stream_.is_open()) stream_.flush();
  }

  // Next entry of the search (Xenia XFile::QueryDirectory): a non-empty
  // pattern restarts from the beginning with that pattern.
  uint32_t NextEntry(std::string_view pattern, bool restart, DirEntry* out, uint32_t* index) {
    std::lock_guard g(mutex_);
    if (!pattern.empty()) {
      pattern_ = std::string(pattern);
      restart = true;
    } else if (!listed_) {
      pattern_ = "*";
      restart = true;
    }
    if (restart || !listed_) {
      ListLocked();
      find_index_ = 0;
    }
    while (find_index_ < entries_.size()) {
      const DirEntry& e = entries_[find_index_++];
      if (MatchPattern(pattern_, e.name)) {
        *out = e;
        *index = uint32_t(find_index_);
        return kStatusSuccess;
      }
    }
    return pattern.empty() ? kStatusNoMoreFiles : kStatusNoSuchFile;
  }

 private:
  bool OpenStreamLocked() {
    stream_.close();
    stream_.clear();
    auto mode = std::ios::binary | std::ios::in;
    if (mount_writable_ && can_write_) mode |= std::ios::out;
    stream_.open(host_, mode);
    return stream_.is_open();
  }

  void ListLocked() {
    entries_.clear();
    listed_ = true;
    std::error_code ec;
    for (fs::directory_iterator it(host_, ec), end; !ec && it != end; it.increment(ec)) {
      const auto u8 = it->path().filename().u8string();
      std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());
      // names the guest could not open (non-ASCII) are left out
      if (std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x7F; }))
        continue;
      DirEntry e;
      e.name = std::move(name);
      std::error_code ec2;
      e.directory = it->is_directory(ec2);
      e.size = e.directory ? 0 : it->file_size(ec2);
      if (ec2) e.size = 0;
      const auto t = it->last_write_time(ec2);
      e.write_time = ec2 ? 0 : ToFileTime(t);
      entries_.push_back(std::move(e));
    }
    std::sort(entries_.begin(), entries_.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
  }

  const std::string guest_;
  const fs::path host_;
  const uint64_t min_size_;
  const bool directory_, mount_writable_, can_read_, can_write_, synchronous_;

  std::mutex mutex_;
  std::fstream stream_;
  uint64_t position_ = 0;
  std::vector<DirEntry> entries_;
  bool listed_ = false;
  size_t find_index_ = 0;
  std::string pattern_;
};

// Attributes, sizes and times of a host path (Xenia HostPathEntry).
struct Stat {
  bool directory = false;
  uint64_t size = 0;
  uint64_t write_time = 0;
  uint32_t attributes = 0;
};
bool StatHost(const fs::path& p, bool mount_writable, Stat* out) {
  std::error_code ec;
  const auto st = fs::status(p, ec);
  if (ec || !fs::exists(st)) return false;
  out->directory = fs::is_directory(st);
  out->size = out->directory ? 0 : fs::file_size(p, ec);
  if (ec) out->size = 0;
  const auto t = fs::last_write_time(p, ec);
  out->write_time = ec ? 0 : ToFileTime(t);
  // FILE_ATTRIBUTE_NORMAL stands alone: read-only -> READONLY
  out->attributes = out->directory ? kAttrDirectory : (mount_writable ? kAttrNormal : kAttrReadOnly);
  return true;
}

// ---- helpers --------------------------------------------------------------------

std::shared_ptr<FileObject> FileFromHandle(uint32_t handle) {
  return std::dynamic_pointer_cast<FileObject>(HandleGet(handle));
}

// Text of a guest ANSI_STRING; nullopt if the buffer is missing or if the
// structure or the buffer exceed the end of the guest space.
std::optional<std::string> AnsiText(uint32_t ansi_addr) {
  if (!GuestRangeValid(ansi_addr, sizeof(AnsiString))) return std::nullopt;
  const AnsiString* s = GuestPtr<AnsiString>(ansi_addr);
  if (!s) return std::nullopt;
  const uint16_t len = s->length;
  if (len == 0) return std::string();
  const uint32_t buffer = s->buffer;
  if (!GuestRangeValid(buffer, len)) return std::nullopt;
  const char* buf = GuestPtr<char>(buffer);
  if (!buf) return std::nullopt;
  return std::string(buf, len);
}

void SetIosb(uint32_t iosb_addr, uint32_t status, uint32_t information) {
  if (IoStatusBlock* iosb = GuestPtr<IoStatusBlock>(iosb_addr)) {
    iosb->status = status;
    iosb->information = information;
  }
}

// Full guest path of an OBJECT_ATTRIBUTES (RootDirectory relative to
// an open directory). The status is the one to return on failure.
std::optional<std::string> PathFromAttributes(const ObjectAttributes* oa, uint32_t* status) {
  auto name = AnsiText(oa->name);
  if (!name) {
    *status = kStatusObjectNameInvalid;
    return std::nullopt;
  }
  const uint32_t root = oa->root_directory;
  if (root == 0 || root == kObDosDevices) return name;
  auto dir = FileFromHandle(root);
  if (!dir) {
    *status = kStatusInvalidHandle;
    LogIo("RootDirectory " + Hex(root) + " is not a directory handle");
    return std::nullopt;
  }
  if (!dir->directory()) {
    *status = kStatusNotADirectory;
    return std::nullopt;
  }
  return dir->guest() + "\\" + *name;
}

uint32_t StatusForResolveError(ResolveError e) {
  return e == ResolveError::kUnknownDevice ? kStatusObjectPathNotFound : kStatusObjectNameInvalid;
}

void LogRejected(const char* fn, std::string_view path, ResolveError e) {
  std::string shown;
  for (char c : path) shown += (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) ? '?' : c;
  LogIo(std::string(fn) + ": path " +
        (e == ResolveError::kUnknownDevice ? "on unknown device" : "invalid or outside the root") +
        " \"" + shown + "\"");
}

// Completion of an I/O (Xenia NtReadFile_entry/NtWriteFile_entry):
// IO_STATUS_BLOCK, completion APC queued to the calling thread with
// ApcRoutine(ApcContext, IoStatusBlock, 0) (the routine's low bit is
// ignored, like Xenia), event signaled. Returns STATUS_PENDING for async
// files (the real status is in the IO_STATUS_BLOCK), otherwise the status.
// Deviation: Xenia queues the APC only with ApcContext != 0; here the
// routine is enough (NT calls it anyway).
uint32_t Complete(bool synchronous, const std::shared_ptr<Event>& ev, uint32_t apc_routine, uint32_t apc_context,
                  uint32_t iosb, uint32_t status, uint32_t information) {
  SetIosb(iosb, status, information);
  if (const uint32_t routine = apc_routine & ~1u) {
    if (GuestThread* t = GuestThread::Current())
      t->QueueApc(routine, apc_context, iosb, 0);
    else
      LogIo("completion routine " + Hex(routine) + " requested by a host thread: ignored");
  }
  if (ev) ev->Set();
  return synchronous ? status : kStatusPending;
}

// Optional event of NtReadFile/NtWriteFile/NtQueryDirectoryFile.
bool EventFromHandle(uint32_t handle, std::shared_ptr<Event>* out) {
  if (!handle) return true;
  *out = std::dynamic_pointer_cast<Event>(HandleGet(handle));
  return *out != nullptr;
}

uint64_t ByteOffset(uint32_t addr) {
  const be<uint64_t>* p = GuestPtr<be<uint64_t>>(addr);
  return p ? uint64_t(*p) : kUseFilePointer;
}

// ---- open -----------------------------------------------------------------------

// Xenia NtCreateFile_entry + VirtualFileSystem::OpenFile.
// Deviations: missing file/path -> STATUS_OBJECT_NAME_NOT_FOUND /
// STATUS_OBJECT_PATH_NOT_FOUND like NT (Xenia: STATUS_NO_SUCH_FILE);
// hostile paths -> STATUS_OBJECT_NAME_INVALID; creating, overwriting or
// superseding on a read-only mount -> STATUS_ACCESS_DENIED. Write access
// to an existing read-only file is downgraded to read
// (like Xenia: games open disc files with GENERIC_WRITE) and
// the actual write fails later.
uint32_t NtCreateFileImpl(be<uint32_t>* handle_out, uint32_t desired_access, ObjectAttributes* oa, uint32_t iosb,
                          be<uint64_t>* /*allocation_size*/, uint32_t /*file_attributes*/, uint32_t /*share_access*/,
                          uint32_t disposition, uint32_t create_options) {
  auto fail = [&](uint32_t status, uint32_t action) {
    if (handle_out) *handle_out = kInvalidHandleValue;
    SetIosb(iosb, status, action);
    return status;
  };
  if (!oa) return kStatusInvalidParameter;  // Xenia: some games do this
  if (disposition > kOverwriteIf) return fail(kStatusInvalidParameter, 0);

  uint32_t status = 0;
  auto path = PathFromAttributes(oa, &status);
  if (!path) return fail(status, 0);
  ResolveError err;
  auto r = ResolveDetailed(*path, &err);
  if (!r) {
    LogRejected("NtCreateFile", *path, err);
    return fail(StatusForResolveError(err), kDoesNotExist);
  }

  const bool want_dir = create_options & kFileDirectoryFile;
  const bool want_file = create_options & kFileNonDirectoryFile;
  const bool synchronous = create_options & (kFileSynchronousIoAlert | kFileSynchronousIoNonalert);
  const bool can_read = desired_access & (kGenericRead | kGenericAll | kFileReadData);
  bool can_write = desired_access & (kGenericWrite | kGenericAll | kFileWriteData | kFileAppendData);

  Stat st;
  const bool exists = r->exists && StatHost(r->host, r->writable, &st);
  st.size = std::max(st.size, r->min_size);
  if (exists) {
    if (st.directory && want_file) return fail(kStatusFileIsADirectory, 0);
    if (!st.directory && want_dir) return fail(kStatusNotADirectory, 0);
  }

  // Existence required / forbidden by the disposition.
  std::error_code ec;
  if (!exists) {
    if (disposition == kOpen || disposition == kOverwrite) {
      const bool parent = fs::is_directory(r->host.parent_path(), ec);
      if (stats::Trace()) LogLine("[io] NtCreateFile " + r->guest + " -> not found");
      return fail(parent ? kStatusObjectNameNotFound : kStatusObjectPathNotFound, kDoesNotExist);
    }
  } else if (disposition == kCreate) {
    return fail(kStatusObjectNameCollision, kExists);
  }

  const bool replaces = exists && (disposition == kSupersede || disposition == kOverwrite || disposition == kOverwriteIf);
  if (!r->writable) {
    if (!exists || replaces) {
      LogIo("NtCreateFile: creation/overwrite denied on read-only " + r->guest);
      return fail(kStatusAccessDenied, 0);
    }
    if (can_write) {
      static std::atomic<bool> once{false};
      if (!once.exchange(true)) LogIo("NtCreateFile: write access downgraded to read on " + r->guest);
      can_write = false;
    }
  }

  // Creation or supersede on the writable mount.
  Action action = kOpened;
  bool directory = exists && st.directory;
  if (!exists || replaces) {
    if (replaces && st.directory) return fail(kStatusAccessDenied, 0);  // a directory is not overwritten
    if (!fs::is_directory(r->host.parent_path(), ec)) return fail(kStatusObjectPathNotFound, kDoesNotExist);
    if (want_dir && !exists) {
      if (!fs::create_directory(r->host, ec) && ec) return fail(kStatusAccessDenied, 0);
      directory = true;
    } else {
      std::ofstream create(r->host, std::ios::binary | std::ios::trunc);
      if (!create) return fail(kStatusAccessDenied, 0);
    }
    InvalidateDirectory(r->host.parent_path());
    action = !exists ? kCreated : (disposition == kSupersede ? kSuperseded : kOverwritten);
  }

  auto file = std::make_shared<FileObject>(r->guest, r->host, directory, r->writable, can_read, can_write, synchronous,
                                          r->min_size);
  if (!directory && !file->OpenStream()) {
    LogIo("NtCreateFile: host open failed for " + r->guest);
    return fail(kStatusAccessDenied, 0);
  }
  if (!directory && IsPackagePath(r->guest)) stats::NotePackageOpened(r->guest);
  const uint32_t handle = HandleCreate(std::move(file));
  if (handle_out) *handle_out = handle;
  SetIosb(iosb, kStatusSuccess, action);
  if (stats::Trace()) LogLine("[io] NtCreateFile " + r->guest + " -> handle " + Hex(handle));
  return kStatusSuccess;
}

// Xenia NtOpenFile_entry: NtCreateFile with FILE_OPEN.
uint32_t NtOpenFileImpl(be<uint32_t>* handle_out, uint32_t desired_access, ObjectAttributes* oa, uint32_t iosb,
                        uint32_t open_options) {
  return NtCreateFileImpl(handle_out, desired_access, oa, iosb, nullptr, 0, 0, kOpen, open_options);
}

// ---- read and write -----------------------------------------------------------------

// Xenia NtReadFile_entry: synchronous read, then completion.
uint32_t NtReadFileImpl(uint32_t file_handle, uint32_t event_handle, uint32_t apc_routine, uint32_t apc_context,
                        uint32_t iosb, uint32_t buffer, uint32_t length, uint32_t byte_offset) {
  std::shared_ptr<Event> ev;
  auto file = FileFromHandle(file_handle);
  if (!file || !EventFromHandle(event_handle, &ev)) {
    LogIo("NtReadFile: unknown handle (file " + Hex(file_handle) + ", event " + Hex(event_handle) + ")");
    SetIosb(iosb, kStatusInvalidHandle, 0);
    return kStatusInvalidHandle;
  }
  uint32_t done = 0;
  uint32_t status;
  if (!GuestRangeValid(buffer, length))
    status = kStatusAccessViolation;
  else {
    // The GPU may observe the destination physical pages (read-only):
    // they are invalidated first, so the kernel-mode copy does not fault. No
    // kernel lock held: GlobalLock is the innermost.
    if (length != 0 && buffer >= 0xA0000000u) physwatch::Trigger(gow2::PhysicalAddressOf(buffer), length, false);
    status = file->Read(GuestPtr<uint8_t>(buffer), length, ByteOffset(byte_offset), &done);
  }
  return Complete(file->synchronous(), ev, apc_routine, apc_context, iosb, status, done);
}

// Xenia NtWriteFile_entry. Read-only mount -> STATUS_ACCESS_DENIED.
uint32_t NtWriteFileImpl(uint32_t file_handle, uint32_t event_handle, uint32_t apc_routine, uint32_t apc_context,
                         uint32_t iosb, uint32_t buffer, uint32_t length, uint32_t byte_offset) {
  std::shared_ptr<Event> ev;
  auto file = FileFromHandle(file_handle);
  if (!file || !EventFromHandle(event_handle, &ev)) {
    LogIo("NtWriteFile: unknown handle (file " + Hex(file_handle) + ", event " + Hex(event_handle) + ")");
    SetIosb(iosb, kStatusInvalidHandle, 0);
    return kStatusInvalidHandle;
  }
  uint32_t done = 0;
  uint32_t status;
  if (!GuestRangeValid(buffer, length))
    status = kStatusAccessViolation;
  else
    status = file->Write(GuestPtr<uint8_t>(buffer), length, ByteOffset(byte_offset), &done);
  if (status == kStatusAccessDenied) LogIo("NtWriteFile: write denied on " + file->guest());
  return Complete(file->synchronous(), ev, apc_routine, apc_context, iosb, status, done);
}

// Xenia NtFlushBuffersFile_entry (a stub that always succeeds); here the stream is flushed.
uint32_t NtFlushBuffersFileImpl(uint32_t file_handle, uint32_t iosb) {
  auto file = FileFromHandle(file_handle);
  if (!file) return kStatusInvalidHandle;
  file->Flush();
  SetIosb(iosb, kStatusSuccess, 0);
  return kStatusSuccess;
}

// ---- information ----------------------------------------------------------------------

// Xenia GetQueryFileInfoMinimumLength (xboxkrnl_io_info.cc), plus
// FileStandardInformation (24 bytes), which Xenia does not handle.
uint32_t QueryFileInfoMinimumLength(uint32_t info_class) {
  switch (info_class) {
    case kFileInternalInformation:
    case kFilePositionInformation:
      return 8;
    case kFileXctdCompressionInformation:
      return 4;
    case kFileNetworkOpenInformation:
      return sizeof(NetworkOpenInformation);
    case kFileModeInformation:
    case kFileAlignmentInformation:
    case kFileSectorInformation:
    case kFileIoPriorityInformation:
      return 4;
    case kFileNameInformation:
    case kFileAllocationInformation:
      return 8;
    case kFileBasicInformation:
      return sizeof(BasicInformation);
    case kFileStandardInformation:
      return sizeof(StandardInformation);
    default:
      return 0;
  }
}

// Xenia NtQueryInformationFile_entry.
uint32_t NtQueryInformationFileImpl(uint32_t file_handle, uint32_t iosb, uint32_t info_addr, uint32_t info_length,
                                    uint32_t info_class) {
  const uint32_t minimum = QueryFileInfoMinimumLength(info_class);
  if (!minimum) return kStatusInvalidInfoClass;
  if (info_length < minimum) return kStatusInfoLengthMismatch;
  auto file = FileFromHandle(file_handle);
  if (!file) return kStatusInvalidHandle;
  uint8_t* info = GuestPtr<uint8_t>(info_addr);
  if (!info || !GuestRangeValid(info_addr, info_length)) return kStatusAccessViolation;
  std::memset(info, 0, info_length);

  Stat st;
  if (!StatHost(file->host(), file->mount_writable(), &st)) st.directory = file->directory();
  st.size = std::max(st.size, file->min_size());
  uint32_t status = kStatusSuccess;
  uint32_t out = 0;
  switch (info_class) {
    case kFileInternalInformation:  // Xenia: hash of the path
      *reinterpret_cast<be<uint64_t>*>(info) = uint64_t(std::hash<std::string>{}(file->guest()));
      out = 8;
      break;
    case kFilePositionInformation:
      *reinterpret_cast<be<uint64_t>*>(info) = file->position();
      out = 8;
      break;
    case kFileNetworkOpenInformation: {
      auto* i = reinterpret_cast<NetworkOpenInformation*>(info);
      i->creation_time = st.write_time;
      i->last_access_time = st.write_time;
      i->last_write_time = st.write_time;
      i->change_time = st.write_time;
      i->allocation_size = AllocationSize(st.size);
      i->end_of_file = st.size;
      i->attributes = st.attributes;
      out = sizeof(*i);
      break;
    }
    case kFileBasicInformation: {
      auto* i = reinterpret_cast<BasicInformation*>(info);
      i->creation_time = st.write_time;
      i->last_access_time = st.write_time;
      i->last_write_time = st.write_time;
      i->change_time = st.write_time;
      i->attributes = st.attributes;
      out = sizeof(*i);
      break;
    }
    case kFileStandardInformation: {
      auto* i = reinterpret_cast<StandardInformation*>(info);
      i->allocation_size = AllocationSize(st.size);
      i->end_of_file = st.size;
      i->number_of_links = 1;
      i->directory = st.directory ? 1 : 0;
      out = sizeof(*i);
      break;
    }
    case kFileAlignmentInformation:  // Xenia: 0 (FILE_BYTE_ALIGNMENT)
      out = 4;
      break;
    case kFileModeInformation:
      *reinterpret_cast<be<uint32_t>*>(info) = file->synchronous() ? kFileSynchronousIoNonalert : 0;
      out = 4;
      break;
    case kFileIoPriorityInformation:
      out = 4;
      break;
    // First sector of the file on its device, used by the XDK's XGetFilePhysicalSortKey to
    // order reads by disc position. Host files have no disc layout, so the key is a constant 0.
    // Xenia returns STATUS_INVALID_PARAMETER here; success avoids the caller's error path
    // (SetLastError, key -1) on every async read and costs nothing.
    case kFileSectorInformation:
      out = 4;
      break;
    default:  // XctdCompression, Name, Allocation: like Xenia, unsupported
      LogIo("NtQueryInformationFile: class " + std::to_string(info_class) + " unsupported");
      status = kStatusInvalidParameter;
      break;
  }
  SetIosb(iosb, status, out);
  return status;
}

// Xenia GetSetFileInfoMinimumLength.
uint32_t SetFileInfoMinimumLength(uint32_t info_class) {
  switch (info_class) {
    case kFileDispositionInformation:
      return 1;
    case kFilePositionInformation:
    case kFileCompletionInformation:
      return 8;
    case kFileModeInformation:
    case kFileIoPriorityInformation:
      return 4;
    case kFileAllocationInformation:
    case kFileEndOfFileInformation:
    case kFileMountPartitionInformation:
      return 8;
    case kFileRenameInformation:
    case kFileLinkInformation:
      return 16;
    case kFileBasicInformation:
      return 40;
    case kFileMountPartitionsInformation:
      return 152;
    default:
      return 0;
  }
}

// Xenia NtSetInformationFile_entry. Deviations: the classes we cannot
// perform (rename, link, partitions) return STATUS_NOT_IMPLEMENTED,
// logged, instead of a silent success; FileCompletionInformation (completion
// ports, not implemented) -> STATUS_INVALID_HANDLE, logged.
uint32_t NtSetInformationFileImpl(uint32_t file_handle, uint32_t iosb, uint32_t info_addr, uint32_t info_length,
                                  uint32_t info_class) {
  const uint32_t minimum = SetFileInfoMinimumLength(info_class);
  if (!minimum) return kStatusInvalidInfoClass;
  if (info_length < minimum) return kStatusInfoLengthMismatch;
  auto file = FileFromHandle(file_handle);
  if (!file) return kStatusInvalidHandle;
  const uint8_t* info = GuestPtr<uint8_t>(info_addr);
  if (!info) return kStatusAccessViolation;

  uint32_t status = kStatusSuccess;
  uint32_t out = 0;
  switch (info_class) {
    case kFilePositionInformation:
      file->set_position(*reinterpret_cast<const be<uint64_t>*>(info));
      out = 8;
      break;
    case kFileEndOfFileInformation:
      status = file->SetLength(*reinterpret_cast<const be<uint64_t>*>(info));
      if (status == kStatusSuccess) InvalidateDirectory(file->host().parent_path());
      out = 8;
      break;
    case kFileDispositionInformation:  // Xenia: delete on close ignored
      LogIo("NtSetInformationFile: deletion of " + file->guest() + " ignored");
      break;
    case kFileAllocationInformation:  // Xenia: ignored
      out = 8;
      break;
    case kFileBasicInformation:
    case kFileModeInformation:
    case kFileIoPriorityInformation:
      break;
    case kFileCompletionInformation:
      LogIo("NtSetInformationFile: completion ports unsupported");
      status = kStatusInvalidHandle;
      break;
    default:
      LogIo("NtSetInformationFile: class " + std::to_string(info_class) + " not implemented");
      status = kStatusNotImplemented;
      break;
  }
  SetIosb(iosb, status, out);
  return status;
}

// Xenia NtQueryFullAttributesFile_entry. Deviation: missing ->
// STATUS_OBJECT_NAME_NOT_FOUND like NT (Xenia: STATUS_NO_SUCH_FILE).
uint32_t NtQueryFullAttributesFileImpl(ObjectAttributes* oa, NetworkOpenInformation* info) {
  if (!oa) return kStatusInvalidParameter;
  uint32_t status = 0;
  auto path = PathFromAttributes(oa, &status);
  if (!path) return status;
  ResolveError err;
  auto r = ResolveDetailed(*path, &err);
  if (!r) {
    LogRejected("NtQueryFullAttributesFile", *path, err);
    return StatusForResolveError(err);
  }
  Stat st;
  if (!r->exists || !StatHost(r->host, r->writable, &st)) {
    std::error_code ec;
    return fs::is_directory(r->host.parent_path(), ec) ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
  }
  st.size = std::max(st.size, r->min_size);
  if (info) {
    *info = {};
    info->creation_time = st.write_time;
    info->last_access_time = st.write_time;
    info->last_write_time = st.write_time;
    info->change_time = st.write_time;
    info->allocation_size = AllocationSize(st.size);
    info->end_of_file = st.size;
    info->attributes = st.attributes;
  }
  return kStatusSuccess;
}

// Valid pattern for NtQueryDirectoryFile (Xenia IsValidPath with
// is_pattern): printable ASCII, no separators.
bool ValidPattern(std::string_view s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c >= 0x7F) return false;
    if (c == '\\' || c == '/' || c == ':' || c == '"' || c == '<' || c == '>' || c == '|') return false;
  }
  return true;
}

// Xenia NtQueryDirectoryFile_entry + XFile::QueryDirectory: one entry per
// call. Deviation: Information = bytes written (Xenia: length), event and
// APC handled as for NtReadFile.
uint32_t NtQueryDirectoryFileImpl(uint32_t file_handle, uint32_t event_handle, uint32_t apc_routine,
                                  uint32_t apc_context, uint32_t iosb, uint32_t info_addr, uint32_t length,
                                  uint32_t file_name, uint32_t restart_scan) {
  if (length < 72) return kStatusInfoLengthMismatch;
  std::string pattern;
  if (file_name) {
    auto text = AnsiText(file_name);
    if (!text || !ValidPattern(*text)) return kStatusInvalidParameter;
    pattern = std::move(*text);
  }
  std::shared_ptr<Event> ev;
  auto file = FileFromHandle(file_handle);
  if (!file || !EventFromHandle(event_handle, &ev)) {
    SetIosb(iosb, kStatusInvalidHandle, 0);
    return kStatusInvalidHandle;
  }
  if (!file->directory()) {
    SetIosb(iosb, kStatusInvalidParameter, 0);
    return kStatusInvalidParameter;
  }
  DirEntry e;
  uint32_t index = 0;
  uint32_t status = file->NextEntry(pattern, restart_scan != 0, &e, &index);
  uint32_t out = 0;
  if (status == kStatusSuccess) {
    const uint32_t needed = uint32_t(offsetof(DirectoryInformation, file_name) + e.name.size());
    if (needed > length) {
      status = kStatusNoSuchFile;  // Xenia: name that does not fit in the buffer
    } else {
      auto* info = GuestPtr<DirectoryInformation>(info_addr);
      if (!info) return kStatusAccessViolation;
      std::memset(info, 0, offsetof(DirectoryInformation, file_name));
      info->next_entry_offset = 0;
      info->file_index = index;
      info->creation_time = e.write_time;
      info->last_access_time = e.write_time;
      info->last_write_time = e.write_time;
      info->change_time = e.write_time;
      info->end_of_file = e.size;
      info->allocation_size = AllocationSize(e.size);
      info->attributes = e.directory ? kAttrDirectory : (file->mount_writable() ? kAttrNormal : kAttrReadOnly);
      info->file_name_length = uint32_t(e.name.size());
      std::memcpy(info->file_name, e.name.data(), e.name.size());
      out = needed;
    }
  }
  return Complete(true, ev, apc_routine, apc_context, iosb, status, out);
}

// Xenia GetQueryVolumeInfoMinimumLength + NtQueryVolumeInformationFile_entry.
// HostPathDevice values (vfs/devices/host_path_device.h): 128K units of
// one 512-byte sector, component names up to 40 characters.
// Deviations: file system name "GDFX" (read-only) / "FATX" instead of the
// device name; FileFsDeviceInformation filled in (CD-ROM or
// disk type) instead of an empty success.
uint32_t NtQueryVolumeInformationFileImpl(uint32_t file_handle, uint32_t iosb, uint32_t info_addr,
                                          uint32_t info_length, uint32_t info_class) {
  uint32_t minimum = 0;
  switch (info_class) {
    case kFileFsVolumeInformation: minimum = sizeof(FsVolumeInformation); break;
    case kFileFsSizeInformation: minimum = sizeof(FsSizeInformation); break;
    case kFileFsAttributeInformation: minimum = sizeof(FsAttributeInformation); break;
    case kFileFsDeviceInformation: minimum = 8; break;
    default: return kStatusInvalidInfoClass;
  }
  if (info_length < minimum) return kStatusInfoLengthMismatch;
  auto file = FileFromHandle(file_handle);
  if (!file) return kStatusInvalidHandle;
  uint8_t* info = GuestPtr<uint8_t>(info_addr);
  if (!info || !GuestRangeValid(info_addr, info_length)) return kStatusAccessViolation;
  std::memset(info, 0, info_length);

  uint32_t status = kStatusSuccess;
  uint32_t out = 0;
  switch (info_class) {
    case kFileFsVolumeInformation:
      out = uint32_t(offsetof(FsVolumeInformation, label));
      break;
    case kFileFsSizeInformation: {
      auto* i = reinterpret_cast<FsSizeInformation*>(info);
      i->total_allocation_units = 128 * 1024;
      i->available_allocation_units = 128 * 1024;
      i->sectors_per_allocation_unit = 1;
      i->bytes_per_sector = 0x200;
      out = sizeof(*i);
      break;
    }
    case kFileFsAttributeInformation: {
      auto* i = reinterpret_cast<FsAttributeInformation*>(info);
      const std::string_view name = file->mount_writable() ? "FATX" : "GDFX";
      i->attributes = 0;
      i->component_name_max_length = 40;
      i->name_length = uint32_t(name.size());
      if (info_length >= offsetof(FsAttributeInformation, name) + name.size()) {
        std::memcpy(i->name, name.data(), name.size());
        out = uint32_t(offsetof(FsAttributeInformation, name) + name.size());
      } else {
        status = kStatusBufferOverflow;
        out = uint32_t(offsetof(FsAttributeInformation, name));
      }
      break;
    }
    case kFileFsDeviceInformation:
      reinterpret_cast<be<uint32_t>*>(info)[0] = file->mount_writable() ? 0x07u : 0x02u;  // DISK / CD_ROM
      out = 8;
      break;
  }
  SetIosb(iosb, status, out);
  return status;
}

// Xenia: FscSetCacheElementCount_entry (xboxkrnl_io.cc, stub): size
// of the file system cache; no cache to size, success.
uint32_t FscSetCacheElementCountImpl(uint32_t, uint32_t) { return 0; }

// Xenia NtDeviceIoControlFile_entry handles two XMountUtilityDrive IOCTLs;
// here none (the game does not need them): STATUS_NOT_IMPLEMENTED, logged by code.
uint32_t NtDeviceIoControlFileImpl(uint32_t handle, uint32_t, uint32_t, uint32_t, uint32_t iosb,
                                   uint32_t io_control_code, uint32_t, uint32_t, uint32_t, uint32_t) {
  static std::mutex m;
  static std::vector<uint32_t> seen;
  {
    std::lock_guard g(m);
    if (std::find(seen.begin(), seen.end(), io_control_code) == seen.end()) {
      seen.push_back(io_control_code);
      LogLine("[io] NtDeviceIoControlFile(handle " + Hex(handle) + ", ioctl " + Hex(io_control_code) +
              ") not implemented");
    }
  }
  SetIosb(iosb, kStatusNotImplemented, 0);
  return kStatusNotImplemented;
}

// ---- symbolic links and volumes ---------------------------------------------------------

// Xenia ObCreateSymbolicLink_entry (xboxkrnl_ob.cc).
uint32_t ObCreateSymbolicLinkImpl(uint32_t link_ansi, uint32_t target_ansi) {
  auto link = AnsiText(link_ansi);
  auto target = AnsiText(target_ansi);
  if (!link || !target) return kStatusInvalidParameter;
  if (!AddSymbolicLink(*link, *target)) {
    LogIo("ObCreateSymbolicLink: invalid name");
    return kStatusObjectNameInvalid;
  }
  if (stats::Trace()) LogLine("[io] ObCreateSymbolicLink " + *link + " -> " + *target);
  return kStatusSuccess;
}

// Xenia ObDeleteSymbolicLink_entry. Deviation: unknown link ->
// STATUS_OBJECT_NAME_NOT_FOUND (Xenia: STATUS_UNSUCCESSFUL).
uint32_t ObDeleteSymbolicLinkImpl(uint32_t link_ansi) {
  auto link = AnsiText(link_ansi);
  if (!link) return kStatusInvalidParameter;
  return RemoveSymbolicLink(*link) ? kStatusSuccess : kStatusObjectNameNotFound;
}

// Xenia does not implement IoDismountVolumeByName (only xboxkrnl_table.inc): our
// mounts are host folders, dismounting = forgetting the cached listings.
uint32_t IoDismountVolumeByNameImpl(uint32_t name_ansi) {
  auto name = AnsiText(name_ansi);
  if (stats::Trace()) LogLine("[io] IoDismountVolumeByName " + name.value_or("?"));
  InvalidateAllDirectories();
  return kStatusSuccess;
}

}  // namespace

KERNEL_EXPORT(NtCreateFile, NtCreateFileImpl)
KERNEL_EXPORT(NtOpenFile, NtOpenFileImpl)
KERNEL_EXPORT(NtReadFile, NtReadFileImpl)
KERNEL_EXPORT(NtWriteFile, NtWriteFileImpl)
KERNEL_EXPORT(NtQueryInformationFile, NtQueryInformationFileImpl)
KERNEL_EXPORT(NtSetInformationFile, NtSetInformationFileImpl)
KERNEL_EXPORT(NtQueryFullAttributesFile, NtQueryFullAttributesFileImpl)
KERNEL_EXPORT(NtQueryDirectoryFile, NtQueryDirectoryFileImpl)
KERNEL_EXPORT(NtQueryVolumeInformationFile, NtQueryVolumeInformationFileImpl)
KERNEL_EXPORT(NtFlushBuffersFile, NtFlushBuffersFileImpl)
KERNEL_EXPORT(NtDeviceIoControlFile, NtDeviceIoControlFileImpl)
KERNEL_EXPORT(FscSetCacheElementCount, FscSetCacheElementCountImpl)
KERNEL_EXPORT(ObCreateSymbolicLink, ObCreateSymbolicLinkImpl)
KERNEL_EXPORT(ObDeleteSymbolicLink, ObDeleteSymbolicLinkImpl)
KERNEL_EXPORT(IoDismountVolumeByName, IoDismountVolumeByNameImpl)
