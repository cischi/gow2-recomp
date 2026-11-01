#include "launcher/game_setup.h"

#include <chrono>
#include <exception>
#include <system_error>

#include "run_extract.h"

namespace launcher {

IsoCheck CheckIso(const std::filesystem::path& iso, uint64_t free_bytes, std::string_view expected) {
  const auto payload = IsoPayloadBytes(iso);
  if (!payload) return IsoCheck::kNotAnIso;
  const auto xex = ReadIsoFile(iso, "default.xex");
  if (!xex) return IsoCheck::kNoXex;
  if (game_data::XexImageSha256(*xex) != expected) return IsoCheck::kWrongEdition;
  constexpr uint64_t kSlack = 256ull << 20;
  if (free_bytes < *payload + kSlack) return IsoCheck::kNoSpace;
  return IsoCheck::kOk;
}

bool DataReady(const std::filesystem::path& dir, std::string_view expected) {
  return game_data::ExtractionComplete(dir) && game_data::CheckXex(dir, expected) == game_data::XexCheck::kOk;
}

uint64_t FreeSpaceFor(const std::filesystem::path& dir) {
  std::error_code ec;
  auto p = std::filesystem::absolute(dir, ec);
  while (!p.empty() && !std::filesystem::exists(p, ec)) {
    const auto parent = p.parent_path();
    if (parent == p) return UINT64_MAX;
    p = parent;
  }
  const auto info = std::filesystem::space(p, ec);
  return ec ? UINT64_MAX : info.available;
}

void Extraction::Start(const std::filesystem::path& iso, const std::filesystem::path& out) {
  done = total = 0;
  cancel = false;
  log.str({});
  result = std::async(std::launch::async, [this, iso, out] {
    ExtractHooks hooks;
    hooks.progress = [this](uint64_t d, uint64_t t) {
      done = d;
      total = t;
    };
    hooks.cancelled = [this] { return cancel.load(); };
    // An exception here (path conversions of disc names) would be rethrown by result.get() and kill the launcher.
    try {
      return RunExtract(iso, out, log, hooks);
    } catch (const std::exception& e) {
      log << "error: unexpected failure: " << e.what() << "\n";
      return 1;
    }
  });
}

bool Extraction::Finished() const {
  return result.valid() && result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

int Extraction::CancelAndJoin() {
  if (!running()) return -1;
  cancel = true;
  return result.get();
}

std::string Extraction::LastLogLine() const {
  std::string text = log.str();
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  const size_t nl = text.find_last_of('\n');
  return nl == std::string::npos ? text : text.substr(nl + 1);
}

}  // namespace launcher
