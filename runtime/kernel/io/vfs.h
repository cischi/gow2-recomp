// Virtual file system: guest devices mounted on host folders,
// case-insensitive path resolution and symbolic
// links (ObCreateSymbolicLink).
//
// Device and link semantics from Xenia
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/vfs/virtual_file_system.cc (ResolvePath, RegisterSymbolicLink,
// FindSymbolicLink) and src/xenia/vfs/devices/host_path_device.cc.
//
// Security (Review Focus 1): a guest path must never escape the
// mount root. Resolve rejects (nullopt):
//   - control characters (NUL included), non-ASCII and `"*<>?|`;
//   - `:` after the device (drive letters `C:`, alternate streams
//     `file::$DATA`, `\\?\`, `\\.\`);
//   - Windows reserved names (CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$,
//     CONOUT$, even with an extension) and components ending with `.` or
//     space (Win32 trims them: `...`, `.. `);
//   - any `..` that climbs above the root;
//   - a result that, once host symbolic links and junctions are resolved
//     (std::filesystem::weakly_canonical), is not under the canonical root.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kernel::io {

// File system return codes (Xenia src/xenia/xbox.h).
inline constexpr uint32_t kStatusPending = 0x103;
inline constexpr uint32_t kStatusBufferOverflow = 0x80000005;
inline constexpr uint32_t kStatusNoMoreFiles = 0x80000006;
inline constexpr uint32_t kStatusNotImplemented = 0xC0000002;
inline constexpr uint32_t kStatusInvalidInfoClass = 0xC0000003;
inline constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004;
inline constexpr uint32_t kStatusAccessViolation = 0xC0000005;
inline constexpr uint32_t kStatusNoSuchFile = 0xC000000F;
inline constexpr uint32_t kStatusEndOfFile = 0xC0000011;
inline constexpr uint32_t kStatusAccessDenied = 0xC0000022;
inline constexpr uint32_t kStatusObjectNameInvalid = 0xC0000033;
inline constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
inline constexpr uint32_t kStatusObjectNameCollision = 0xC0000035;
inline constexpr uint32_t kStatusObjectPathNotFound = 0xC000003A;
inline constexpr uint32_t kStatusFileIsADirectory = 0xC00000BA;
inline constexpr uint32_t kStatusNotADirectory = 0xC0000103;

// A guest device ("game:", "d:", "\Device\Cdrom0", "cache:") mounted
// on a host folder. `device` without a trailing separator.
struct Mount {
  std::string device;
  std::filesystem::path root;
  bool writable;
};

// Replaces the mounts (and clears symbolic links and directory caches).
// The roots are made absolute and canonical. Thread-safe.
void SetMounts(std::vector<Mount>);
// Adds (or replaces, same device) a mount, without touching the
// others nor the links (e.g. XAM content packages).
void AddMount(Mount);

// Removes the `device` mount (same comparison as AddMount, case-
// insensitive); false if it was not there. Symbolic links and caches stay.
bool RemoveMount(std::string_view device);

// true if `name` is a single path component the VFS accepts (same
// rules as Resolve: printable ASCII without `"*<>?|:`, not a Windows
// reserved name, does not end with `.` or space, allowed length). Separators
// (backslash and slash) are not allowed. XAM uses it for content names.
bool IsValidComponent(std::string_view name);

// Host path of a guest path; nullopt if outside the root, hostile or
// on an unknown device. The file may not exist (creation): missing
// components stay as written by the guest.
std::optional<std::filesystem::path> Resolve(std::string_view guest_path);

enum class ResolveError { kNone, kInvalidName, kUnknownDevice };

struct Resolution {
  std::filesystem::path host;  // host path (under the mount root)
  std::string guest;           // normalized guest path: device + "\Comp\Comp" with the host's case
  bool writable = false;       // read/write mount
  bool exists = false;         // all components found on the host
  uint64_t min_size = 0;       // replaced file (--skip-movie): size of the original, zero tail
};
// Like Resolve, with details; *error says why it fails.
std::optional<Resolution> ResolveDetailed(std::string_view guest_path, ResolveError* error = nullptr);

// File replacements by name (--skip-movie): a path whose last
// component is `from` (case-insensitive) opens `to`: a name in the
// same folder (--skip-movie) or an absolute host path chosen by the
// runtime (engine Coalesced; read-only, applies only to read-only
// mounts: a namesake in cache: stays the game's file). Resolution::min_size = size
// of `from` in both cases (the game reads some files in full according to the
// expected size). Replaces the list;
// SetMounts does not touch it.
void SetFileReplacements(std::vector<std::pair<std::string, std::string>> from_to);

// Symbolic links (Xenia ObCreateSymbolicLink_entry / ObDeleteSymbolicLink_entry):
// `link` without the leading "\??\" (removed if present), `target` a guest path
// (device or link, possibly with subfolders). false if a name is not
// valid (empty, disallowed characters) or, for Delete, if the link is not there.
bool AddSymbolicLink(std::string_view link, std::string_view target);
bool RemoveSymbolicLink(std::string_view link);

// Forgets the cached listing of `host_dir` (after creation, deletion or
// renaming of one of its children); without an argument clears the whole cache.
void InvalidateDirectory(const std::filesystem::path& host_dir);
void InvalidateAllDirectories();

// true if the name has extension .xxx, .upk or .tfc (case-
// insensitive): opening it counts as a package in the summary.
bool IsPackagePath(std::string_view guest_path);

// Compares a name against an NtQueryDirectoryFile pattern (`*`, `?`,
// "*.*" = everything), case-insensitive.
bool MatchPattern(std::string_view pattern, std::string_view name);

}  // namespace kernel::io
