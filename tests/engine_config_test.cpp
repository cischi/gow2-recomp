// Coalesced format, [SystemSettings] edits and generated files. Synthetic bytes only.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "engine_config.h"
#include "temp_dir.h"

using namespace engine_config;
using settings::Engine;
using settings::ShadowQuality;
namespace fs = std::filesystem;

namespace {
using Bytes = std::vector<uint8_t>;

Bytes I32(int32_t n) {
  const uint32_t u = uint32_t(n);
  return {uint8_t(u >> 24), uint8_t(u >> 16), uint8_t(u >> 8), uint8_t(u)};
}
Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes o;
  for (const auto& p : parts) o.insert(o.end(), p.begin(), p.end());
  return o;
}
Bytes W(std::u16string_view s) {
  Bytes o = I32(-int32_t(s.size() + 1));
  for (char16_t c : s) o.insert(o.end(), {uint8_t(c & 0xFF), uint8_t(c >> 8)});
  o.insert(o.end(), {0, 0});
  return o;
}
Bytes A(std::string_view s) {
  Bytes o = I32(int32_t(s.size() + 1));
  o.insert(o.end(), s.begin(), s.end());
  o.push_back(0);
  return o;
}
Bytes KV(std::u16string_view k, std::u16string_view v) { return Cat({W(k), W(v)}); }

constexpr std::u16string_view kTuple =
    u"(MinLODSize=256,MaxLODSize=1024,LODBias=1,MinMagFilter=aniso,MipFilter=point)";

Bytes Sample(bool with_dynamic = true, std::u16string_view tuple = kTuple) {
  Bytes sys = Cat({W(u"SystemSettings"), I32(with_dynamic ? 5 : 4)});
  auto add = [&](const Bytes& b) { sys.insert(sys.end(), b.begin(), b.end()); };
  if (with_dynamic) add(KV(u"DynamicShadows", u"True"));
  add(KV(u"MaxShadowResolution", u"800"));
  add(KV(u"DetailMode", u"2"));
  add(KV(u"DetailMode", u"3"));
  add(KV(u"TEXTUREGROUP_World", tuple));
  const Bytes f1 = Cat({W(u"..\\GearGame\\Config\\Xe-GearEngine.ini"), I32(3),
                        W(u"Engine.Engine"), I32(1), KV(u"MaxShadowResolution", u"432"), sys,
                        W(u"SystemSettingsEditor"), I32(1), KV(u"TEXTUREGROUP_World", kTuple)});
  const Bytes f2 = Cat({A("..\\GearGame\\Localization\\RUS\\Gear.int"), I32(1), W(u"Тест"),
                        I32(1), W(u"Key"), I32(0)});
  return Cat({I32(2), f1, f2});
}

std::u16string Val(const std::vector<IniFile>& f, size_t sec, std::u16string_view key, size_t nth = 0) {
  for (const auto& e : f[0].sections[sec].entries)
    if (e.key.text == key && nth-- == 0) return e.value.text;
  return u"<missing>";
}

void WriteFile(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
}  // namespace

TEST_CASE("engine_config: Parse and Serialize round-trip", "[engine_config]") {
  const Bytes b = Sample();
  const auto f = Parse(b);
  REQUIRE(f);
  CHECK(Serialize(*f) == b);
  REQUIRE(f->size() == 2);
  CHECK((*f)[0].name.text == u"..\\GearGame\\Config\\Xe-GearEngine.ini");
  CHECK((*f)[0].name.wide);
  CHECK_FALSE((*f)[1].name.wide);
  CHECK((*f)[1].sections[0].entries[0].value.zero);
  CHECK(Val(*f, 1, u"DetailMode", 1) == u"3");
}

TEST_CASE("engine_config: a string with only the trailing zero and n == 0 stay distinct", "[engine_config]") {
  const Bytes b = Cat({I32(1), A(""), I32(1), W(u""), I32(2), A(""), I32(0), W(u"k"), A("")});
  const auto f = Parse(b);
  REQUIRE(f);
  CHECK(Serialize(*f) == b);
}

TEST_CASE("engine_config: Parse ignores the tail", "[engine_config]") {
  Bytes b = Sample();
  const Bytes orig = b;
  b.insert(b.end(), 16, 0);
  const auto f = Parse(b);
  REQUIRE(f);
  CHECK(Serialize(*f) == orig);
}

TEST_CASE("engine_config: Parse rejects invalid files", "[engine_config]") {
  CHECK_FALSE(Parse({}));
  const Bytes b = Sample();
  for (size_t n = 1; n < b.size(); ++n)
    CHECK_FALSE(Parse(std::span<const uint8_t>(b.data(), n)));
  CHECK_FALSE(Parse(I32(-1)));
  CHECK_FALSE(Parse(Cat({I32(1), A("x"), I32(1), A("s"), I32(0x7FFFFFFF)})));
  CHECK_FALSE(Parse(Cat({I32(1), I32(-0x40000000)})));
  CHECK_FALSE(Parse(Cat({I32(1), I32(INT32_MIN)})));
  CHECK_FALSE(Parse(Cat({I32(1), I32(2), Bytes{'a', 'b'}, I32(0)})));  // the trailing zero is missing
}

TEST_CASE("engine_config: EditsFor", "[engine_config]") {
  CHECK(EditsFor(Engine{}).empty());
  auto e = EditsFor(Engine{ShadowQuality::kLow, true});
  REQUIRE(e.size() == 1);
  CHECK(e[0].key == "MaxShadowResolution");
  CHECK(e[0].field.empty());
  CHECK(e[0].value == "256");
  e = EditsFor(Engine{ShadowQuality::kHigh, true});
  CHECK(e[0].value == "2048");
  e = EditsFor(Engine{ShadowQuality::kOriginal, false});
  REQUIRE(e.size() == 1);
  CHECK(e[0].key == "DynamicShadows");
  CHECK(e[0].value == "False");
  e = EditsFor(Engine{ShadowQuality::kHigh, false});
  REQUIRE(e.size() == 2);
  CHECK(e[0].key == "MaxShadowResolution");
  CHECK(e[1].key == "DynamicShadows");
}

// Edit of a field of a tuple "(A=..,B=..)": no option uses it any more (world_textures was
// removed), it remains available in ApplyEdits.
const Edit kLodBias0{"TEXTUREGROUP_World", "LODBias", "0"};

TEST_CASE("engine_config: ApplyEdits changes only [SystemSettings]", "[engine_config]") {
  auto files = *Parse(Sample());
  auto edits = EditsFor(Engine{ShadowQuality::kHigh, false});
  edits.push_back(kLodBias0);
  const auto r = ApplyEdits(files, edits);
  REQUIRE(r);
  CHECK(*r == std::vector<std::string>{"MaxShadowResolution 800 -> 2048", "DynamicShadows True -> False",
                                       "TEXTUREGROUP_World LODBias 1 -> 0"});
  CHECK(Val(files, 1, u"MaxShadowResolution") == u"2048");
  CHECK(Val(files, 1, u"DynamicShadows") == u"False");
  CHECK(Val(files, 1, u"TEXTUREGROUP_World") ==
        u"(MinLODSize=256,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=point)");
  CHECK(Val(files, 0, u"MaxShadowResolution") == u"432");
  CHECK(Val(files, 2, u"TEXTUREGROUP_World") == kTuple);
  CHECK(files[0].sections[1].entries[1].value.wide);
  const auto again = Parse(Serialize(files));
  REQUIRE(again);
  CHECK(*again == files);
}

TEST_CASE("engine_config: ApplyEdits with a missing key or field", "[engine_config]") {
  auto files = *Parse(Sample(false));
  const auto before = files;
  const auto r = ApplyEdits(files, EditsFor(Engine{ShadowQuality::kHigh, false}));
  REQUIRE_FALSE(r);
  CHECK(r.error() == "Xe-GearEngine.ini [SystemSettings] DynamicShadows missing");
  CHECK(files == before);

  files = *Parse(Sample(true, u"(MinLODSize=256,MaxLODSize=1024,MinMagFilter=aniso)"));
  const auto b2 = files;
  const auto r2 = ApplyEdits(files, std::vector<Edit>{kLodBias0});
  REQUIRE_FALSE(r2);
  CHECK(r2.error() == "Xe-GearEngine.ini [SystemSettings] TEXTUREGROUP_World LODBias missing");
  CHECK(files == b2);

  files = *Parse(Sample(true, u"(MinLODSize=256,XLODBias=1)"));
  CHECK_FALSE(ApplyEdits(files, std::vector<Edit>{kLodBias0}));
}

TEST_CASE("engine_config: Prepare writes only the valid modified Coalesced files", "[engine_config]") {
  gow2_test::TempDir tmp("engcfg");
  const fs::path cooked = tmp.path / "cooked", out = tmp.path / "out";
  WriteFile(cooked / "Coalesced_int.bin", Sample());
  WriteFile(cooked / "COALESCED_ITA.BIN", Sample());
  WriteFile(cooked / "Coalesced_bad.bin", {1, 2, 3});
  WriteFile(cooked / "Other.bin", Sample());

  const auto p = Prepare(cooked, out, Engine{ShadowQuality::kHigh, true});
  REQUIRE(p.replacements.size() == 2);
  CHECK(p.replacements[0].first == "COALESCED_ITA.BIN");
  CHECK(p.replacements[1].first == "Coalesced_int.bin");
  for (const auto& [name, path] : p.replacements) {
    CHECK(fs::path(path).is_absolute());
    CHECK(fs::path(path) == fs::absolute(out / name));
    std::ifstream in(path, std::ios::binary);
    const Bytes b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto f = Parse(b);
    REQUIRE(f);
    CHECK(Val(*f, 1, u"MaxShadowResolution") == u"2048");
  }
  for (const auto& e : fs::directory_iterator(out)) CHECK(e.path().extension() != ".tmp");
  CHECK(std::distance(fs::directory_iterator(out), fs::directory_iterator{}) == 2);

  const auto has = [&](std::string_view s) { return std::find(p.log.begin(), p.log.end(), s) != p.log.end(); };
  CHECK(has("[engine] Coalesced_int.bin: MaxShadowResolution 800 -> 2048"));
  CHECK(has("[engine] Coalesced_bad.bin: invalid format, original"));
  CHECK(p.log.back().starts_with("[engine] 2 Coalesced modified in "));

  gow2_test::TempDir tmp2("engcfg");
  const auto q = Prepare(cooked, tmp2.path / "out", Engine{});
  CHECK(q.replacements.empty());
  CHECK_FALSE(fs::exists(tmp2.path / "out"));
}

TEST_CASE("engine_config: ApplyEdits finds the field case-insensitively", "[engine_config]") {
  auto files = *Parse(Sample(true, u"(MinLODSize=32,lodbias=1)"));
  const auto r = ApplyEdits(files, std::vector<Edit>{kLodBias0});
  REQUIRE(r);
  CHECK(*r == std::vector<std::string>{"TEXTUREGROUP_World LODBias 1 -> 0"});
  CHECK(Val(files, 1, u"TEXTUREGROUP_World") == u"(MinLODSize=32,lodbias=0)");
}

TEST_CASE("engine_config: Prepare with names outside the code page", "[engine_config]") {
  gow2_test::TempDir tmp("engcfg");
  const fs::path cooked = tmp.path / L"cooked_中";
  WriteFile(cooked / "Coalesced_int.bin", Sample());
  WriteFile(cooked / L"Coalesced_中.bin", {1, 2, 3});
  const auto p = Prepare(cooked, tmp.path / "out", Engine{ShadowQuality::kHigh, true});
  CHECK(p.replacements.size() == 1);
  const auto bad = std::count_if(p.log.begin(), p.log.end(),
                                 [](const std::string& l) { return l.find("invalid format, original") != std::string::npos; });
  CHECK(bad == 1);
}

TEST_CASE("engine_config: Prepare with an unreadable folder logs a read failure", "[engine_config]") {
  gow2_test::TempDir tmp("engcfg");
  const auto p = Prepare(tmp.path / "missing", tmp.path / "out",
                         Engine{ShadowQuality::kHigh, true});
  CHECK(p.replacements.empty());
  REQUIRE(p.log.size() == 1);
  CHECK(p.log[0].starts_with("[engine] "));
  CHECK(p.log[0].ends_with(": read failed, original"));
}
