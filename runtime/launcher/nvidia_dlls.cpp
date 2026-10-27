#include "launcher/nvidia_dlls.h"

#include <vector>

#include "game_data.h"

namespace nvidia_dlls {

namespace fs = std::filesystem;

std::optional<std::string> Install(const fs::path& exe_dir, const optiscaler::Fetch& fetch,
                                   std::span<const Dll> dlls) {
  optiscaler::ResetCancel();
  std::error_code ec;
  std::vector<fs::path> staged;
  std::vector<const Dll*> todo;
  const auto cleanup = [&] {
    for (const auto& p : staged) fs::remove(p, ec);
  };
  for (const Dll& d : dlls) {
    const fs::path final_path = exe_dir / std::string(d.name);
    if (game_data::Sha256File(final_path) == std::string(d.sha256)) continue;
    const fs::path part = exe_dir / (std::string(d.name) + ".download");
    staged.push_back(part);
    todo.push_back(&d);
    if (const auto err = fetch(d.url, part)) {
      cleanup();
      return std::string(d.name) + ": " + *err;
    }
    if (game_data::Sha256File(part) != std::string(d.sha256)) {
      cleanup();
      return std::string(d.name) + ": SHA-256 mismatch";
    }
  }
  // shortcut: a rename failing midway (DLL in use) leaves earlier DLLs installed; harmless, retry completes it.
  for (size_t i = 0; i < todo.size(); ++i) {
    fs::rename(staged[i], exe_dir / std::string(todo[i]->name), ec);
    if (ec) {
      const std::string msg = std::string(todo[i]->name) + ": " + ec.message();
      cleanup();
      return msg;
    }
  }
  return std::nullopt;
}

}  // namespace nvidia_dlls
