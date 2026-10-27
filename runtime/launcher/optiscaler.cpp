#include "launcher/optiscaler.h"

#include "game_data.h"

#include <windows.h>

#include <winhttp.h>
#include <winver.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace optiscaler {

namespace fs = std::filesystem;

namespace {

// Backup copies of the replaced files, inside exe_dir (rename on the same volume), one subfolder per
// installation. kKeepMarker: restore failed, SweepOld does not touch it.
constexpr const char* kOldDir = ".gow2_optiscaler_old";
constexpr const char* kKeepMarker = "RESTORE_NEEDED.txt";
constexpr const char* kCancelled = "installation cancelled";

// CancelInstall / WaitInstallCopy (see optiscaler.h).
std::atomic<bool> g_cancel{false};
std::mutex g_copy;  // held by Install during copy and restore

std::string Utf8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// ProductName of the version block (first language of the Translation table).
std::wstring ProductName(const fs::path& p) {
  const DWORD size = GetFileVersionInfoSizeW(p.c_str(), nullptr);
  if (size == 0) return {};
  std::vector<char> info(size);
  if (!GetFileVersionInfoW(p.c_str(), 0, size, info.data())) return {};
  const WORD* lang = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", (void**)&lang, &len) || len < 4) return {};
  wchar_t key[64];
  swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\ProductName", lang[0], lang[1]);
  const wchar_t* name = nullptr;
  if (!VerQueryValueW(info.data(), key, (void**)&name, &len) || len == 0) return {};
  return name;
}

// Temporary folder of the installation, removed with all its contents on exit.
struct ScopedDir {
  fs::path path;
  ~ScopedDir() {
    std::error_code ec;
    if (!path.empty()) fs::remove_all(path, ec);
  }
};

// Windows' tar.exe (bsdtar, reads zips): exit code, or -1 if it does not start.
long RunTar(const fs::path& zip, const fs::path& out) {
  wchar_t sys[MAX_PATH];
  if (GetSystemDirectoryW(sys, MAX_PATH) == 0) return -1;
  const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + out.wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                      &pi))
    return -1;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<long>(code);
}

// Writes to exe_dir: on the first error Rollback deletes the new files and puts the backup copies back.
struct Copier {
  fs::path backup_dir;
  std::vector<fs::path> created_files, created_dirs;
  std::vector<std::pair<fs::path, fs::path>> saved;  // (destination, backup copy)

  // Throws filesystem_error. The copy goes to <dst>.new first and is then renamed: an interruption leaves no
  // truncated file. The existing destination is renamed into backup_dir (inside exe_dir, same volume): a mapped
  // DLL (OptiScaler's dxgi.dll loaded in the game) cannot be deleted but can be renamed.
  void Place(const fs::path& src, const fs::path& dst) {
    fs::path fresh = dst;
    fresh += ".new";
    created_files.push_back(fresh);  // before the copy: a half-done copy must be deleted
    fs::copy_file(src, fresh, fs::copy_options::overwrite_existing);
    if (fs::exists(dst)) {
      fs::create_directories(backup_dir);
      const fs::path bk = backup_dir / std::to_string(saved.size());
      fs::rename(dst, bk);
      saved.emplace_back(dst, bk);
    }
    created_files.push_back(dst);
    fs::rename(fresh, dst);
  }
  void MakeDir(const fs::path& dir) {
    if (fs::create_directory(dir)) created_dirs.push_back(dir);
  }
  // false if a backup copy does not go back to its place: backup_dir stays, marked for SweepOld.
  bool Rollback() {
    std::error_code ec;
    for (auto it = created_files.rbegin(); it != created_files.rend(); ++it) fs::remove(*it, ec);
    bool ok = true;
    for (const auto& [dst, bk] : saved) {
      fs::rename(bk, dst, ec);
      ok = ok && !ec;
    }
    for (auto it = created_dirs.rbegin(); it != created_dirs.rend(); ++it) fs::remove(*it, ec);
    if (ok) {
      fs::remove(backup_dir, ec);
      fs::remove(backup_dir.parent_path(), ec);  // only if empty
    } else {
      std::ofstream(backup_dir / kKeepMarker) << "Previous OptiScaler files were not restored: put them back by hand.\n";
    }
    return ok;
  }
};

// Default settings written at installation: every key must exist in the
// given section. Preset=2 is the small model (DLL 310.9 only); F6/F7 = 0x75/0x76.
struct Preset { std::string_view section, key, value; };
constexpr Preset kPresets[] = {
    {"[Upscalers]", "Dx12Upscaler", "dlss"},
    {"[FrameGen]", "Enabled", "true"},
    {"[FrameGen]", "FGInput", "upscaler"},
    {"[FrameGen]", "FGOutput", "dlssg"},
    {"[FSRFG]", "AllowAsync", "true"},
    {"[MFGUnlock]", "RTX40", "detect"},
    {"[Menu]", "FGShortcutKey", "0x76"},
    {"[DlssNr]", "Enabled", "true"},
    {"[DlssNr]", "RunBeforeSR", "true"},
    {"[DlssNr]", "Preset", "2"},
    {"[DlssNr]", "ReversibleMode", "7"},
    {"[DlssNr]", "WorkingScale", "0.75"},
    {"[DlssNr]", "SkinStructure", "0.75"},
    {"[DlssNr]", "LocalTone", "0.0"},
    {"[DlssNr]", "ToggleKey", "0x75"},
};

}  // namespace

State Detect(const fs::path& exe_dir, std::string_view dll_sha256) {
  const fs::path dxgi = exe_dir / "dxgi.dll";
  std::error_code ec;
  if (!fs::exists(dxgi, ec)) return State::kMissing;
  if (game_data::Sha256File(dxgi) == dll_sha256) return State::kInstalled;
  return ProductName(dxgi) == L"OptiScaler" ? State::kOther : State::kForeign;
}

bool Loaded(const fs::path& exe_dir) {
  const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
  if (!dxgi) return false;
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(dxgi, path, MAX_PATH);
  if (n == 0 || n == MAX_PATH) return false;
  std::error_code ec;
  return fs::equivalent(fs::path(path).parent_path(), exe_dir, ec);
}

std::string PatchIni(std::string_view ini) {
  std::string out;
  out.reserve(ini.size() + 64);
  std::string_view section;
  bool done[std::size(kPresets)] = {};
  while (!ini.empty()) {
    const size_t nl = ini.find('\n');
    const size_t end = nl == std::string_view::npos ? ini.size() : nl + 1;
    std::string_view line = ini.substr(0, end);
    ini.remove_prefix(end);
    // The fork uses LF; a trailing CR (CRLF file) stays where it is.
    size_t body = line.size();
    if (body > 0 && line[body - 1] == '\n') --body;
    if (body > 0 && line[body - 1] == '\r') --body;
    const std::string_view text = line.substr(0, body);
    if (text.starts_with('[')) section = text.substr(0, text.find(']') + 1);
    // "Key=value" or "Key = value" (OptiScaler rewrites the ini saved from its menu this way).
    const size_t eq = text.find('=');
    bool patched = false;
    if (eq != std::string_view::npos && !text.starts_with(';')) {
      std::string_view key = text.substr(0, eq);
      while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
      for (size_t i = 0; i < std::size(kPresets) && !patched; ++i) {
        if (section != kPresets[i].section || key != kPresets[i].key) continue;
        size_t v = eq + 1;
        if (v < text.size() && text[v] == ' ' && text.substr(0, eq) != key) ++v;  // "Key = value"
        out += text.substr(0, v);
        out += kPresets[i].value;
        out += line.substr(body);
        done[i] = patched = true;
      }
    }
    if (!patched) out += line;
  }
  for (bool d : done)
    if (!d) return std::string();  // unexpected file: empty, Install refuses
  return out;
}

std::optional<std::string> HttpFetch(std::wstring_view url, const fs::path& dest) {
  std::wstring u(url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(u.c_str(), 0, 0, &parts)) return "invalid address";
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                            std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

  std::optional<std::string> error = "network unavailable";
  HINTERNET session = WinHttpOpen(L"gow2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (session) WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);  // a network interruption does not block closing
  HINTERNET connect = session ? WinHttpConnect(session, host.c_str(), parts.nPort, 0) : nullptr;
  HINTERNET request =
      connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
              : nullptr;
  if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      error = "HTTP " + std::to_string(status);
    } else {
      std::ofstream out(dest, std::ios::binary);
      std::vector<char> buf(1 << 16);
      DWORD got = 0;
      bool ok = static_cast<bool>(out);
      while (ok && !g_cancel.load() && WinHttpReadData(request, buf.data(), static_cast<DWORD>(buf.size()), &got) && got > 0)
        ok = static_cast<bool>(out.write(buf.data(), got));
      // got == 0 at the end of the body; otherwise the read was interrupted or the disk is full.
      if (ok && got == 0) error.reset();
      else error = "download interrupted";
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
  return error;
}

std::optional<std::string> Install(const fs::path& exe_dir, const InstallConfig& cfg) {
  ResetCancel();
  if (Detect(exe_dir, cfg.dll_sha256) == State::kForeign)
    return "there is already a dxgi.dll that is not OptiScaler: not replacing it";
  SweepOld(exe_dir);

  std::error_code ec;
  // Leftovers of an exit via _Exit during the download (the ScopedDir did not run): only those of processes
  // no longer alive are removed (another launcher may be installing).
  for (const auto& e : fs::directory_iterator(fs::temp_directory_path(ec), ec)) {
    const std::string n = Utf8(e.path().filename());
    if (!n.starts_with("gow2_optiscaler_")) continue;
    char* end = nullptr;
    const DWORD pid = static_cast<DWORD>(std::strtoul(n.c_str() + 16, &end, 10));
    if (end == n.c_str() + 16 || *end != '_' || pid == GetCurrentProcessId()) continue;  // only <pid>_<tick>
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
      const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
      CloseHandle(h);
      if (alive) continue;
    }
    fs::remove_all(e.path(), ec);
  }
  ScopedDir tmp;
  tmp.path = fs::temp_directory_path(ec) /
             ("gow2_optiscaler_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
  if (ec || !fs::create_directories(tmp.path / "x", ec))
    return "temporary folder unavailable";

  const fs::path zip = tmp.path / "pkg.zip";
  const auto fetch_err = cfg.fetch(cfg.url, zip);
  if (g_cancel.load()) return kCancelled;
  if (fetch_err) return "download failed: " + *fetch_err;
  if (game_data::Sha256File(zip) != cfg.zip_sha256) return "the downloaded zip has a different SHA-256 than expected";

  const fs::path x = tmp.path / "x";
  if (const long code = RunTar(zip, x); code != 0)
    return "unpacking the zip failed (tar: " + std::to_string(code) + ")";
  for (const char* name : {"OptiScaler.dll", "OptiScaler.ini", "nvngx.dll_dlssnr.dll"})
    if (!fs::is_regular_file(x / name, ec)) return std::string("the zip is missing ") + name;
  if (!fs::is_directory(x / "OptiScaler", ec)) return "the zip is missing the OptiScaler folder";

  {
    std::ifstream in(x / "OptiScaler.ini", std::ios::binary);
    const std::string ini{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::string patched = PatchIni(ini);
    if (patched.empty()) return "unexpected OptiScaler.ini";
    in.close();
    std::ofstream out(x / "OptiScaler.ini", std::ios::binary | std::ios::trunc);
    if (!(out << patched)) return "OptiScaler.ini cannot be modified";
  }

  // Check under the lock: whoever closes after CancelInstall() + WaitInstallCopy() leaves no half-done copy.
  std::unique_lock copying(g_copy);
  if (g_cancel.load()) return kCancelled;
  Copier copier{exe_dir / kOldDir / tmp.path.filename()};
  try {
    copier.Place(x / "OptiScaler.ini", exe_dir / "OptiScaler.ini");
    copier.Place(x / "nvngx.dll_dlssnr.dll", exe_dir / "nvngx.dll_dlssnr.dll");
    copier.MakeDir(exe_dir / "OptiScaler");
    for (const auto& entry : fs::recursive_directory_iterator(x / "OptiScaler")) {
      const fs::path dst = exe_dir / "OptiScaler" / fs::relative(entry.path(), x / "OptiScaler");
      if (entry.is_directory()) copier.MakeDir(dst);
      else copier.Place(entry.path(), dst);
    }
    // Last: dxgi.dll decides kInstalled, a copy interrupted before here does not count as installed.
    copier.Place(x / "OptiScaler.dll", exe_dir / "dxgi.dll");
  } catch (const fs::filesystem_error& e) {
    const std::string what = "copying " + Utf8(e.path1().filename()) + " failed";
    if (copier.Rollback()) return what + " (file in use or folder not writable): no file changed";
    return what + " and the restore is incomplete: the previous files are in " + Utf8(copier.backup_dir);
  }
  copying.unlock();
  SweepOld(exe_dir);  // replaced DLLs that are still mapped stay until a later call
  return std::nullopt;
}

void CancelInstall() { g_cancel.store(true); }
void ResetCancel() { g_cancel.store(false); }
bool CancelRequested() { return g_cancel.load(); }

void WaitInstallCopy() { std::lock_guard wait(g_copy); }

void SweepOld(const fs::path& exe_dir) {
  const fs::path old = exe_dir / kOldDir;
  std::error_code ec;
  std::vector<fs::path> runs;
  for (const auto& run : fs::directory_iterator(old, ec)) runs.push_back(run.path());
  for (const auto& run : runs) {
    if (fs::exists(run / kKeepMarker, ec)) continue;
    std::vector<fs::path> files;
    for (const auto& f : fs::directory_iterator(run, ec)) files.push_back(f.path());
    for (const auto& f : files) fs::remove(f, ec);  // a DLL that is still mapped stays: it is retried later
    fs::remove(run, ec);
  }
  fs::remove(old, ec);  // only if empty
}

void OpenMenu(void* hwnd) {
  // lParam like a real keyboard: repeat 1, scan code of INS (0x52), extended key; the release has bits 30-31.
  const LPARAM down = 1 | (0x52 << 16) | (1 << 24);
  const LPARAM up = down | (1 << 30) | (1u << 31);
  PostMessageW(static_cast<HWND>(hwnd), WM_KEYDOWN, VK_INSERT, down);
  PostMessageW(static_cast<HWND>(hwnd), WM_KEYUP, VK_INSERT, up);
}

settings::FrameGeneration EffectiveFrameGeneration(settings::FrameGeneration wanted, State s) {
  return s == State::kInstalled ? settings::FrameGeneration::kOff : wanted;
}

}  // namespace optiscaler
