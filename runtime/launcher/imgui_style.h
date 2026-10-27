// ImGui font and style shared by the launcher and the F1 overlay (current ImGui context).
#pragma once

namespace launcher {

// Segoe UI (%WINDIR%\Fonts\segoeui.ttf) if it exists, otherwise ImGui's font.
void LoadFonts();
// Dark style with spacing and font at the given scale (1 = 96 DPI); can be called again.
void ApplyScale(float scale);

}  // namespace launcher
