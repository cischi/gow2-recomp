// gears.exe model: which game executables sit next to it. No Win32, no ImGui.
#pragma once

#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

namespace dispatcher {

struct Game {
  std::string_view id, name;
};
inline constexpr std::array<Game, 2> kGames{{{"gow2", "Gears of War 2"}, {"gow3", "Gears of War 3"}}};

struct Entry {
  Game game;
  std::filesystem::path exe;
  bool available;
};

// One entry per kGames, in order. exe = dir / "<id>.exe" (absolute); available = it is a regular file.
std::vector<Entry> FindGames(const std::filesystem::path& dir);

}  // namespace dispatcher
