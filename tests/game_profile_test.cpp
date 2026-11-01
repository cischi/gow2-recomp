// game_profile: game profile (games/<id>/profile.cpp) and the file names derived from it. Built once per game:
// game_profile_test links gow2_game, game_profile_gow3_test links gow3_game (GOW3_PROFILE).
#include <catch2/catch_test_macros.hpp>

#include "game_profile.h"

#ifdef GOW3_PROFILE
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

#include "launcher/game_setup.h"
#include "launcher/launcher_model.h"
#include "launcher/launcher_text.h"
#include "temp_dir.h"
#include "xdvdfs_builder.h"

TEST_CASE("gow3_profile", "[game_profile]") {
  const auto& p = game_profile::Get();
  CHECK(p.id == "gow3");
  CHECK(game_profile::SettingsFileName() == "gow3.toml");
  CHECK(game_profile::LogFileName() == "gow3.log");
  CHECK(p.cooked_dir == "GearGame/CookedXbox360");
  CHECK(p.dof_downsample_hash == 0);
  CHECK(p.dof_downsample_2x2_hash == 0xF820776A22228E1Aull);
  CHECK(p.dof_composite_hash == 0x2F284B68A112ED3Eull);
  CHECK(p.shadow_projection_hash == 0x3C5E875D17B49A36ull);
  CHECK(p.shadow_spot_light_hash == 0x857CECDEB64A657Full);
  CHECK(p.shadow_point_light_hash == 0xCA3F4A808B09D6F4ull);
  CHECK(p.dof_blur_downsample_hash == 0x363BCAEF3EB3423Bull);
  CHECK(p.image_sha256 == GAME_EXPECTED_SHA256);
}

TEST_CASE("gow3_profile: every patch option is available, HDR is not", "[game_profile]") {
  // HDR: gpu/hdr_passes.h reconstructs GoW2's uber post; GoW3's tonemapper (filmic + 3D LUT) is another.
  CHECK(launcher::UnavailableOptions() == std::set<std::string>{"hdr"});
  CHECK(std::string(launcher::Text(launcher::UiLanguage::kEnglish).hdr_not_in_game) ==
        "Not available in Gears of War 3: the HDR reconstruction follows another game's tonemapping");
}

TEST_CASE("gow3_profile: post capture of the split post chain", "[game_profile]") {
  const auto& k = game_profile::Get().post;
  CHECK(k.final_ps == 0x2F284B68A112ED3Eull);  // filmic tonemap + 3D LUT, last draw before the HUD
  CHECK(k.depth_ps == 0xF820776A22228E1Aull);  // half-resolution downsample with the scene depth
  CHECK(k.motion_ps == 0x63E1C54E3C4C2596ull);  // half-resolution DoF + motion blur
  CHECK(k.depth_tf == 0);
  CHECK(k.linearize_c == 2);
  CHECK(k.velocity_tf == 1);
  CHECK(k.velocity_fetch_swizzle);  // k_8_8_8_8 read through swizzle zyxw
  CHECK(k.reprojection_c == 8);
  CHECK(k.blur_scale_c == 12);
  CHECK(k.blur_limit_c == game_profile::PostCapture::kNone);  // velocity buffer = velocity * c12.xy
  CHECK(k.blur_off[0] == 12 * 4);
  CHECK(k.blur_off[1] == 12 * 4 + 1);
  CHECK(k.blur_off[2] == 13 * 4 + 2);
  CHECK(k.blur_off[3] == 13 * 4 + 3);
  CHECK_FALSE(k.hdr);
}
TEST_CASE("gow3_profile: launcher texts name Gears of War 3, World and gow3.exe", "[game_profile]") {
  using L = launcher::UiLanguage;
  const auto& it = launcher::Text(L::kItalian);
  const auto& en = launcher::Text(L::kEnglish);
  CHECK(std::string(en.data_missing) == "Game data missing: choose your Gears of War 3 ISO (World / Italy).");
  CHECK(std::string(it.data_missing) == "Dati del gioco mancanti: scegli la tua ISO di Gears of War 3 (World / Italia).");
  CHECK(std::string(en.extract_wrong_edition) == "Unsupported edition: the extracted files do not match the World / Italy ISO.");
  CHECK(std::string(en.frame_generation_unavailable) ==
        "Not available: amd_fidelityfx_framegeneration_dx12.dll is missing next to gow3.exe");
  for (const L l : {L::kItalian, L::kEnglish}) {
    const auto& t = launcher::Text(l);
    for (const char* s : {t.frame_generation_unavailable, t.frame_generation_dlss_unavailable, t.optiscaler_tip,
                          t.optiscaler_no_dlssnr}) {
      CHECK(std::string(s).find("gow3.exe") != std::string::npos);
      CHECK(std::string(s).find("gow2.exe") == std::string::npos);
    }
  }
}

TEST_CASE("gow3_profile: CheckIso rejects another edition with a Gears of War 3 message", "[game_profile]") {
  gow2_test::TempDir t("gow3_setup_edition");
  const auto img = BuildImage({{"default.xex", "XEX2 of another game"}}, 0xFD90000);
  const auto iso = t.path / "other.iso";
  std::ofstream(iso, std::ios::binary).write(reinterpret_cast<const char*>(img.data()), std::streamsize(img.size()));
  CHECK(launcher::CheckIso(iso, 1ull << 40, game_profile::Get().image_sha256) == launcher::IsoCheck::kWrongEdition);
  // The message the launcher shows for kWrongEdition.
  CHECK(std::string(launcher::Text(launcher::UiLanguage::kEnglish).iso_wrong_edition) ==
        "Unsupported edition: the World / Italy ISO of Gears of War 3 is required.");
  CHECK(std::string(launcher::Text(launcher::UiLanguage::kItalian).iso_wrong_edition) ==
        "Edizione non supportata: serve l'ISO World / Italia di Gears of War 3.");
}

// The launcher's Choose ISO path on the real disc (CheckIso, Extraction, DataReady): label lungo, GOW3_ISO.
TEST_CASE("real ISO gow3", "[real]") {
  const char* iso = std::getenv("GOW3_ISO");
  if (!iso) SKIP("GOW3_ISO not set");
  gow2_test::TempDir t("gow3_real");
  const auto out = t.path / "game_data_gow3";
  const auto& p = game_profile::Get();
  REQUIRE(launcher::CheckIso(iso, launcher::FreeSpaceFor(out), p.image_sha256) == launcher::IsoCheck::kOk);
  CHECK_FALSE(launcher::DataReady(out, p.image_sha256));
  launcher::Extraction ex;
  ex.Start(iso, out);
  REQUIRE(ex.result.get() == 0);
  CHECK(ex.done.load() == ex.total.load());
  CHECK(launcher::DataReady(out, p.image_sha256));
}
#else
TEST_CASE("gow2_profile", "[game_profile]") {
  const auto& p = game_profile::Get();
  CHECK(p.id == "gow2");
  CHECK(game_profile::SettingsFileName() == "gow2.toml");
  CHECK(game_profile::LogFileName() == "gow2.log");
  CHECK(p.image_sha256.size() == 64);
  CHECK(p.image_sha256 == GAME_EXPECTED_SHA256);
  // UE3's uber post does it all in one draw: the values the backend had hard-coded before the per-game data.
  const auto& k = p.post;
  CHECK(k.final_ps == 0xB6B6DD4E7D91F3D9ull);
  CHECK(k.depth_ps == k.final_ps);
  CHECK(k.motion_ps == k.final_ps);
  CHECK(k.size_tf == 0);
  CHECK(k.depth_tf == 0);
  CHECK(k.linearize_c == 2);
  CHECK(k.velocity_tf == 3);
  CHECK_FALSE(k.velocity_fetch_swizzle);  // default view of the velocity buffer
  CHECK(k.reprojection_c == 13);
  CHECK(k.blur_scale_c == 17);
  CHECK(k.blur_limit_c == 17);  // c17 copied as is
  CHECK(k.blur_off[0] == 17 * 4);
  CHECK(k.blur_off[1] == 17 * 4 + 1);
  CHECK(k.blur_off[2] == 18 * 4);
  CHECK(k.blur_off[3] == 18 * 4 + 1);
  CHECK(k.hdr);
  CHECK(k.hdr_scene_tf == 1);
  CHECK(k.hdr_bloom_tf == 2);
  CHECK(p.dof_downsample_hash == 0x0354AF5BA0F333D1ull);
  CHECK(p.dof_downsample_2x2_hash == 0);
  CHECK(p.dof_composite_hash == 0);
  CHECK(p.shadow_projection_hash == 0);
  CHECK(p.shadow_spot_light_hash == 0);
  CHECK(p.shadow_point_light_hash == 0);
  CHECK(p.dof_blur_downsample_hash == 0);
}
#endif
