#include "launcher/imgui_style.h"

#include <windows.h>

#include <imgui.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace launcher {
namespace {
constexpr float kFontSize = 18.0f;
}  // namespace

void LoadFonts() {
  ImGuiIO& io = ImGui::GetIO();
  std::error_code ec;
  wchar_t windir[MAX_PATH];
  const UINT n = GetWindowsDirectoryW(windir, MAX_PATH);
  const std::filesystem::path segoe = std::filesystem::path(std::wstring(windir, n)) / "Fonts" / "segoeui.ttf";
  const std::u8string segoe_u8 = segoe.u8string();
  if (!(n && std::filesystem::exists(segoe, ec) &&
        io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(segoe_u8.c_str()), kFontSize))) {
    // Without Segoe UI the built-in vector font (the bitmap one is sharp only at 13 px with no scaling).
    ImFontConfig cfg;
    cfg.SizePixels = kFontSize;
    io.Fonts->AddFontDefaultVector(&cfg);
  }
}

void ApplyScale(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  ImGui::StyleColorsDark(&style);
  style.WindowPadding = ImVec2(16, 14);
  style.FrameRounding = 4;
  style.ScaleAllSizes(scale);
  style.FontSizeBase = kFontSize;
  style.FontScaleDpi = scale;
}

}  // namespace launcher
