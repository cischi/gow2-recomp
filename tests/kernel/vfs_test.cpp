// Virtual file system (runtime/kernel/io/vfs.*): mounts, case-insensitive
// resolution, mixed slashes, `.`/`..`, hostile paths (Review
// Focus 1), unknown devices, symbolic links, directory caches. On a
// temporary folder: no game bytes.
#include "kernel/io/vfs.h"
#include "../temp_dir.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace kernel::io;

namespace {

void Touch(const fs::path& p, std::string_view content = "x") {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

// game/ (read-only), cache/ (writable), outside/ next to them: the target
// of the traversal attacks.
struct Tree {
  fs::path base, game, cache, outside;
  Tree() {
    base = gow2_test::UniqueTempPath("gow2_vfs_test");  // unique per process (ctest -j)
    game = base / "game";
    cache = base / "cache";
    outside = base / "outside";
    Touch(game / "GearGame" / "CookedXenon" / "Startup.xxx");
    Touch(game / "Readme.TXT");
    Touch(game / "Dir" / "Sub" / "file.bin");
    Touch(outside / "secret.txt");
    fs::create_directories(cache);
    game = fs::weakly_canonical(game);
    cache = fs::weakly_canonical(cache);
    SetMounts({{"game:", game, false},
               {"d:", game, false},
               {"\\Device\\Cdrom0", game, false},
               {"cache:", cache, true}});
  }
  ~Tree() {
    SetMounts({});
    std::error_code ec;
    fs::remove_all(base, ec);
  }
};

bool Same(const std::optional<fs::path>& got, const fs::path& want) {
  if (!got) return false;
  return got->lexically_normal() == want.lexically_normal();
}

}  // namespace

TEST_CASE("vfs: mixed case, mixed slashes and `.` find the file with the host's name") {
  Tree t;
  const fs::path startup = t.game / "GearGame" / "CookedXenon" / "Startup.xxx";
  CHECK(Same(Resolve("game:\\GearGame\\CookedXenon\\Startup.xxx"), startup));
  CHECK(Same(Resolve("GAME:\\geargame\\COOKEDXENON\\startup.XXX"), startup));
  CHECK(Same(Resolve("game:/GearGame\\CookedXenon/startup.xxx"), startup));
  CHECK(Same(Resolve("game:\\.\\GearGame\\.\\CookedXenon\\.\\Startup.xxx"), startup));
  CHECK(Same(Resolve("game:\\\\GearGame\\\\CookedXenon\\Startup.xxx"), startup));
  CHECK(Same(Resolve("\\Device\\Cdrom0\\GEARGAME\\CookedXenon\\Startup.xxx"), startup));
  CHECK(Same(Resolve("\\device\\cdrom0\\GearGame\\CookedXenon\\Startup.xxx"), startup));
  CHECK(Same(Resolve("d:\\readme.txt"), t.game / "Readme.TXT"));
  CHECK(Same(Resolve("\\??\\game:\\readme.txt"), t.game / "Readme.TXT"));
  // `..` that stays inside the root is allowed
  CHECK(Same(Resolve("game:\\GearGame\\..\\Readme.TXT"), t.game / "Readme.TXT"));
  CHECK(Same(Resolve("game:\\Dir\\Sub\\..\\..\\Dir\\SUB\\FILE.BIN"), t.game / "Dir" / "Sub" / "file.bin"));
  // device root
  CHECK(Same(Resolve("game:\\"), t.game));
  CHECK(Same(Resolve("game:"), t.game));

  auto r = ResolveDetailed("game:\\geargame\\cookedxenon\\STARTUP.xxx");
  REQUIRE(r);
  CHECK(r->guest == "game:\\GearGame\\CookedXenon\\Startup.xxx");
  CHECK(r->exists);
  CHECK_FALSE(r->writable);
}

TEST_CASE("vfs: nonexistent file -> path for creation, with the host's existing parts") {
  Tree t;
  auto r = ResolveDetailed("cache:\\New\\Save.dat");
  REQUIRE(r);
  CHECK_FALSE(r->exists);
  CHECK(r->writable);
  CHECK(r->host.lexically_normal() == (t.cache / "New" / "Save.dat").lexically_normal());
  auto g = ResolveDetailed("game:\\GEARGAME\\missing.xxx");
  REQUIRE(g);
  CHECK_FALSE(g->exists);
  CHECK(g->host.lexically_normal() == (t.game / "GearGame" / "missing.xxx").lexically_normal());
}

TEST_CASE("vfs: hostile paths never leave the root (Review Focus 1)") {
  Tree t;
  const char* hostile[] = {
      "game:\\..\\outside\\secret.txt",
      "game:\\..\\..\\x",
      "game:\\..",
      "game:..\\outside\\secret.txt",
      "game:/../outside/secret.txt",
      "GAME:\\GearGame\\..\\..\\outside\\secret.txt",
      "game:\\.\\..\\outside\\secret.txt",
      "game:\\a\\..\\..\\outside\\secret.txt",
      "game:\\GearGame\\CookedXenon\\..\\..\\..\\outside\\secret.txt",
      "game:\\x\\y\\..\\..\\..\\..\\..\\..\\..\\..\\outside",
      "\\Device\\Cdrom0\\GearGame\\..\\..",
      "\\Device\\Cdrom0\\..",
      "\\Device\\Cdrom0/../outside/secret.txt",
      "\\??\\game:\\..\\outside\\secret.txt",
      "cache:\\..\\game\\Readme.TXT",
      // drive letters, UNC, \\?\ and \\.\ prefixes, alternate streams
      "game:\\C:\\Windows\\win.ini",
      "game:C:\\Windows",
      "game:\\\\?\\C:\\Windows",
      "game:\\\\.\\C:",
      "game:\\Readme.TXT::$DATA",
      "game:\\Readme.TXT:stream",
      "game:\\file::$DATA",
      // Windows reserved names
      "game:\\CON",
      "game:\\con.txt",
      "game:\\Dir\\NUL",
      "game:\\Dir\\nul.dat",
      "game:\\AUX",
      "game:\\PRN.log",
      "game:\\COM1",
      "game:\\lpt9.bin",
      "game:\\CONIN$",
      "game:\\conout$",
      // trailing dots and spaces (Win32 trims them), disallowed characters
      "game:\\...",
      "game:\\...\\outside",
      "game:\\.. \\outside\\secret.txt",
      "game:\\. .\\x",
      "game:\\Readme.TXT.",
      "game:\\Readme.TXT ",
      "game:\\*",
      "game:\\Read?e.TXT",
      "game:\\a|b",
      "game:\\a<b",
      "game:\\a>b",
      "game:\\a\"b",
      "game:\\\xC3\xA9t\xC3\xA9",
      "game:\\Readme.TXT\t",
  };
  for (const char* p : hostile) {
    INFO(p);
    ResolveError err = ResolveError::kNone;
    CHECK_FALSE(ResolveDetailed(p, &err));
    CHECK(err == ResolveError::kInvalidName);
    CHECK_FALSE(Resolve(p));
  }
  // NUL byte in the middle (ANSI_STRING with explicit length)
  const std::string with_nul("game:\\Readme.TXT\0\\..\\..\\outside", 31);
  CHECK_FALSE(Resolve(with_nul));
  const std::string just_nul("game:\\\0", 7);
  CHECK_FALSE(Resolve(just_nul));
  // path too long
  std::string long_path = "game:";
  for (int i = 0; i < 400; ++i) long_path += "\\abcdefgh";
  CHECK_FALSE(Resolve(long_path));
  // component too long
  CHECK_FALSE(Resolve("game:\\" + std::string(300, 'a')));
}

TEST_CASE("vfs: host junction pointing outside the root -> rejected") {
  Tree t;
  const fs::path link = t.game / "escape";
  const std::string cmd = "mklink /J \"" + link.string() + "\" \"" + t.outside.string() + "\" >NUL 2>&1";
  if (std::system(cmd.c_str()) != 0 || !fs::exists(link / "secret.txt")) {
    WARN("mklink /J not available: junction test skipped");
    return;
  }
  InvalidateAllDirectories();
  CHECK_FALSE(Resolve("game:\\escape\\secret.txt"));
  CHECK_FALSE(Resolve("game:\\ESCAPE"));
  CHECK_FALSE(Resolve("game:\\escape\\newfile"));
  fs::remove(link);  // removes the junction, not the target
  CHECK(fs::exists(t.outside / "secret.txt"));
}

TEST_CASE("vfs: unknown device or wrong name boundary -> nullopt") {
  Tree t;
  const char* unknown[] = {
      "foo:\\x",
      "dd:\\Readme.TXT",
      "\\Device\\Cdrom01\\Readme.TXT",
      "\\Device\\Harddisk0\\Partition1\\x",
      "C:\\Windows\\win.ini",
      "\\\\?\\C:\\Windows",
      "\\\\server\\share\\x",
      "Readme.TXT",
      "\\Readme.TXT",
      "",
  };
  for (const char* p : unknown) {
    INFO(p);
    ResolveError err = ResolveError::kNone;
    CHECK_FALSE(ResolveDetailed(p, &err));
    CHECK(err != ResolveError::kNone);
    CHECK_FALSE(Resolve(p));
  }
  ResolveError err = ResolveError::kNone;
  CHECK_FALSE(ResolveDetailed("foo:\\x", &err));
  CHECK(err == ResolveError::kUnknownDevice);
}

TEST_CASE("vfs: link simbolici (ObCreateSymbolicLink)") {
  Tree t;
  REQUIRE(AddSymbolicLink("\\??\\alias:", "\\Device\\Cdrom0\\GearGame"));
  CHECK(Same(Resolve("alias:\\cookedxenon\\startup.xxx"), t.game / "GearGame" / "CookedXenon" / "Startup.xxx"));
  CHECK(Same(Resolve("ALIAS:/CookedXenon"), t.game / "GearGame" / "CookedXenon"));
  // `..` from the link: climbs within the mount root, never beyond
  CHECK(Same(Resolve("alias:\\..\\Readme.TXT"), t.game / "Readme.TXT"));
  CHECK_FALSE(Resolve("alias:\\..\\..\\outside\\secret.txt"));
  // link to a link
  REQUIRE(AddSymbolicLink("second:", "alias:\\CookedXenon"));
  CHECK(Same(Resolve("second:\\STARTUP.XXX"), t.game / "GearGame" / "CookedXenon" / "Startup.xxx"));
  // the link wins over a mount with the same name
  REQUIRE(AddSymbolicLink("\\??\\d:", "cache:"));
  CHECK(Same(Resolve("d:\\x.bin"), t.cache / "x.bin"));
  REQUIRE(RemoveSymbolicLink("\\??\\d:"));
  CHECK(Same(Resolve("d:\\Readme.TXT"), t.game / "Readme.TXT"));
  // link pointing into itself: applied only once
  REQUIRE(AddSymbolicLink("self:", "game:"));
  REQUIRE(AddSymbolicLink("game:", "self:\\GearGame"));
  CHECK(Same(Resolve("game:\\CookedXenon\\Startup.xxx"), t.game / "GearGame" / "CookedXenon" / "Startup.xxx"));
  REQUIRE(RemoveSymbolicLink("game:"));
  // cycle: no infinite recursion
  REQUIRE(AddSymbolicLink("loopa:", "loopb:"));
  REQUIRE(AddSymbolicLink("loopb:", "loopa:\\x"));
  CHECK_FALSE(Resolve("loopa:\\y"));
  // link to something that leaves the root
  REQUIRE(AddSymbolicLink("evil:", "game:\\..\\outside"));
  CHECK_FALSE(Resolve("evil:\\secret.txt"));
  // invalid names
  CHECK_FALSE(AddSymbolicLink("", "game:"));
  CHECK_FALSE(AddSymbolicLink("bad\x01:", "game:"));
  CHECK_FALSE(AddSymbolicLink("ok:", std::string_view("ga\0me:", 6)));
  // deletion
  REQUIRE(RemoveSymbolicLink("alias:"));
  CHECK_FALSE(Resolve("alias:\\cookedxenon\\startup.xxx"));
  CHECK_FALSE(RemoveSymbolicLink("alias:"));
  // SetMounts clears the links
  SetMounts({{"game:", t.game, false}});
  CHECK_FALSE(Resolve("second:\\STARTUP.XXX"));
}

TEST_CASE("vfs: directory caches, files created after the first listing") {
  Tree t;
  CHECK_FALSE(ResolveDetailed("cache:\\Later.BIN")->exists);
  Touch(t.cache / "later.bin");
  // writable mount: a missing name rereads the directory
  auto r = ResolveDetailed("cache:\\LATER.bin");
  REQUIRE(r);
  CHECK(r->exists);
  CHECK(r->host.filename() == "later.bin");
  // read-only mount: cached listing until it is invalidated
  CHECK_FALSE(ResolveDetailed("game:\\New.txt")->exists);
  Touch(t.game / "new.txt");
  InvalidateDirectory(t.game);
  CHECK(ResolveDetailed("game:\\NEW.TXT")->exists);
}

TEST_CASE("vfs: AddMount adds a device without touching the others") {
  Tree t;
  REQUIRE(AddSymbolicLink("keep:", "game:"));
  AddMount({"content:", t.cache, true});
  CHECK(Same(Resolve("content:\\x"), t.cache / "x"));
  CHECK(Same(Resolve("keep:\\Readme.TXT"), t.game / "Readme.TXT"));
}

TEST_CASE("vfs: packages and patterns") {
  CHECK(IsPackagePath("game:\\GearGame\\CookedXenon\\Startup.xxx"));
  CHECK(IsPackagePath("game:\\x\\A.UPK"));
  CHECK(IsPackagePath("game:\\x\\Textures.Tfc"));
  CHECK_FALSE(IsPackagePath("game:\\x\\a.xxx.bak"));
  CHECK_FALSE(IsPackagePath("game:\\x\\xxx"));
  CHECK_FALSE(IsPackagePath("game:\\x.xxx\\file"));

  CHECK(MatchPattern("*", "anything.bin"));
  CHECK(MatchPattern("*.*", "noext"));
  CHECK(MatchPattern("*.TXT", "b.txt"));
  CHECK_FALSE(MatchPattern("*.txt", "b.bin"));
  CHECK(MatchPattern("a?c", "ABC"));
  CHECK_FALSE(MatchPattern("a?c", "abbc"));
  CHECK(MatchPattern("Startup*", "startup_loc.xxx"));
  CHECK(MatchPattern("", "x"));
}

TEST_CASE("vfs: RemoveMount unmounts a single device; IsValidComponent") {
  Tree t;
  AddMount({"temp:", t.cache, true});
  AddMount({"temp2:", t.cache, true});
  CHECK(Resolve("temp:/x"));
  CHECK(RemoveMount("TEMP:"));
  CHECK_FALSE(Resolve("temp:/x"));
  CHECK(Resolve("temp2:/x"));
  CHECK_FALSE(RemoveMount("temp:"));
  CHECK(RemoveMount("temp2:"));

  CHECK(IsValidComponent("Slot0"));
  CHECK(IsValidComponent("a b.c"));
  for (const char* bad : {"", ".", "..", "a.", "a ", "CON", "nul.txt", "a:b", "a/b", "a|b", "a*b", "a\"b", "a?b"})
    CHECK_FALSE(IsValidComponent(bad));
  CHECK_FALSE(IsValidComponent(std::string(300, 'a')));
}

TEST_CASE("vfs: SetFileReplacements with an absolute path opens a file outside the mount") {
  Tree t;
  Touch(t.game / "GearGame" / "CookedXenon" / "Coalesced_int.bin", "orig-file");
  const fs::path gen = t.outside / "gen" / "Coalesced_int.bin";
  Touch(gen, "new and longer");
  InvalidateAllDirectories();
  const std::u8string u8 = gen.generic_u8string();
  SetFileReplacements({{"COALESCED_INT.BIN", std::string(u8.begin(), u8.end())}});
  auto r = ResolveDetailed("game:/GearGame/CookedXenon/coalesced_int.bin");
  REQUIRE(r);
  CHECK(r->exists);
  CHECK(r->host.lexically_normal() == gen.lexically_normal());
  CHECK(r->min_size == 9);
  CHECK_FALSE(r->writable);
  CHECK(r->guest == "game:\\GearGame\\CookedXenon\\Coalesced_int.bin");
  // original missing: no replacement, the path stays under the mount
  auto missing = ResolveDetailed("game:/GearGame/Coalesced_int.bin");
  REQUIRE(missing);
  CHECK_FALSE(missing->exists);
  CHECK(missing->host.lexically_normal().string().starts_with(t.game.lexically_normal().string()));
  SetFileReplacements({});
  CHECK(Same(Resolve("game:/GearGame/CookedXenon/coalesced_int.bin"),
             t.game / "GearGame" / "CookedXenon" / "Coalesced_int.bin"));
}

TEST_CASE("vfs: SetFileReplacements with an absolute path only on read-only mounts") {
  Tree t;
  Touch(t.game / "GearGame" / "CookedXenon" / "Coalesced_int.bin", "orig-file");
  Touch(t.cache / "Coalesced_int.bin", "from cache");
  const fs::path gen = t.outside / "gen" / "Coalesced_int.bin";
  Touch(gen, "new and longer");
  InvalidateAllDirectories();
  const std::u8string u8 = gen.generic_u8string();
  SetFileReplacements({{"COALESCED_INT.BIN", std::string(u8.begin(), u8.end())}});
  auto c = ResolveDetailed("cache:/Coalesced_int.bin");
  REQUIRE(c);
  CHECK(c->writable);
  CHECK(c->host.lexically_normal().string().starts_with(t.cache.lexically_normal().string()));
  auto g = ResolveDetailed("game:/GearGame/CookedXenon/Coalesced_int.bin");
  REQUIRE(g);
  CHECK(g->host.lexically_normal() == gen.lexically_normal());
  SetFileReplacements({});
}

TEST_CASE("vfs: SetFileReplacements opens another file in the same folder (--skip-movie)") {
  Tree t;
  Touch(t.game / "GearGame" / "Movies" / "Intro.bik", "long");
  Touch(t.game / "GearGame" / "Movies" / "Loading.bik", "short");
  InvalidateAllDirectories();  // read-only mount: GearGame's listing may already be cached
  SetFileReplacements({{"INTRO.BIK", "Loading.bik"}});
  auto r = ResolveDetailed("game:/GearGame/Movies/intro.bik");
  REQUIRE(r);
  CHECK(r->exists);
  CHECK(r->host.lexically_normal() == (t.game / "GearGame" / "Movies" / "Loading.bik").lexically_normal());
  // only the last component: a folder with that name stays as it is
  SetFileReplacements({{"movies", "Other"}});
  CHECK(Same(Resolve("game:/GearGame/Movies/Intro.bik"), t.game / "GearGame" / "Movies" / "Intro.bik"));
  SetFileReplacements({});
  CHECK(Same(Resolve("game:/GearGame/Movies/Intro.bik"), t.game / "GearGame" / "Movies" / "Intro.bik"));
}
