#include "log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <io.h>
#include <mutex>
#include <string>
#include <thread>

namespace {
std::timed_mutex g_mutex;
// Thread holding g_mutex (empty id if none): the crash path reads it so as not
// to wait for a lock owned by the thread that crashed, instead
// of a try_lock that would lose the line when the lock belongs to another thread.
std::atomic<std::thread::id> g_owner{};
std::ofstream g_file;

// g_mutex taken with the owner recorded.
class OwnedLock {
 public:
  OwnedLock() {
    g_mutex.lock();
    g_owner.store(std::this_thread::get_id());
  }
  ~OwnedLock() {
    g_owner.store(std::thread::id{});
    g_mutex.unlock();
  }
  OwnedLock(const OwnedLock&) = delete;
  OwnedLock& operator=(const OwnedLock&) = delete;
};

void WriteStderr(std::string_view line) {
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void WriteFile(std::string_view line) {
  if (!g_file.is_open()) return;
  g_file.write(line.data(), std::streamsize(line.size()));
  g_file.put('\n');
  g_file.flush();
}
}  // namespace

void LogInit(const std::filesystem::path& path) {
  OwnedLock lock;
  if (g_file.is_open()) g_file.close();
  if (path.empty()) return;
  g_file.open(path, std::ios::binary | std::ios::trunc);
  if (!g_file) {
    const auto u8 = path.u8string();
    WriteStderr("log: cannot open " + std::string(u8.begin(), u8.end()));
  }
}

void LogLine(std::string_view line) {
  OwnedLock lock;
  WriteFile(line);
  WriteStderr(line);
}

void LogLineFromCrash(std::string_view line) {
  // Crash inside LogLine of this same thread: the lock will never be released,
  // the line goes to stderr only. If the lock belongs to another thread we wait
  // for it (it writes one line and releases it), with a limit in case that
  // thread is stuck forever.
  if (g_owner.load() != std::this_thread::get_id() && g_mutex.try_lock_for(std::chrono::seconds(2))) {
    g_owner.store(std::this_thread::get_id());
    WriteFile(line);
    g_owner.store(std::thread::id{});
    g_mutex.unlock();
  }
  // CRT descriptor 2: no lock on the stderr FILE stream
  _write(2, line.data(), unsigned(line.size()));
  _write(2, "\n", 1);
}
