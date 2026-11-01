// gears.exe model (runtime/dispatcher): which game exes sit next to it.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "dispatcher/dispatcher_model.h"
#include "temp_dir.h"

TEST_CASE("dispatcher: FindGames lists kGames in order", "[dispatcher]") {
  gow2_test::TempDir dir("gow2_dispatcher");
  std::ofstream(dir.path / "gow2.exe") << "x";
  const auto games = dispatcher::FindGames(dir.path);
  REQUIRE(games.size() == dispatcher::kGames.size());
  for (size_t i = 0; i < games.size(); ++i) {
    CHECK(games[i].game.id == dispatcher::kGames[i].id);
    CHECK(games[i].exe.is_absolute());
    CHECK(games[i].exe.parent_path() == std::filesystem::absolute(dir.path));
  }
  CHECK(games[0].available);
  CHECK_FALSE(games[1].available);
}

TEST_CASE("dispatcher: empty folder and directory named like an exe", "[dispatcher]") {
  gow2_test::TempDir dir("gow2_dispatcher");
  for (const auto& e : dispatcher::FindGames(dir.path)) CHECK_FALSE(e.available);
  std::filesystem::create_directory(dir.path / "gow3.exe");
  CHECK_FALSE(dispatcher::FindGames(dir.path)[1].available);
}
