#include "dispatcher/dispatcher_model.h"

#include <string>
#include <system_error>

namespace dispatcher {

std::vector<Entry> FindGames(const std::filesystem::path& dir) {
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(dir, ec);
  std::vector<Entry> out;
  for (const Game& g : kGames) {
    std::filesystem::path exe = abs / (std::string(g.id) + ".exe");
    const bool ok = std::filesystem::is_regular_file(exe, ec);
    out.push_back({g, std::move(exe), ok});
  }
  return out;
}

}  // namespace dispatcher
