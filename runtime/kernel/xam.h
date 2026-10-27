// XAM: local user, content, system UI, notifications and language
// (xam_exports.cpp). Configuration and control points used by main and tests.
//
// Semantics from Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/kernel/xam/xam_user.cc, xam_content.cc, xam_content_device.cc,
// xam_enum.cc, xam_notify.cc, xam_ui.cc, xam_info.cc and xnotifylistener.cc.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace kernel::xam {

// Xenia's XLanguage (src/xenia/xbox.h): en=1 ja=2 de=3 fr=4 es=5 it=6 ko=7
// zh=8 (traditional Chinese; "zh-tw" is a synonym, "zh-cn"=10 is simplified)
// pt=9 pl=11 ru=12. The code is compared case-insensitively.
// nullopt if the code is unknown.
std::optional<uint32_t> LanguageFromCode(std::string_view code);
// Language returned by XGetLanguage (default 1 = English).
void SetLanguage(uint32_t language);
uint32_t Language();

// Content folder (user_data/content): each package opened with
// XamContentCreateEx is its subfolder <sanitized name>, mounted under the
// root name chosen by the game. Must be set before using XamContent*.
void SetContentRoot(const std::filesystem::path& root);

// Notifies all listeners that filter it in (area mask and
// version), like Xenia's KernelState::BroadcastNotification.
void BroadcastNotification(uint32_t id, uint32_t data);

// XAM state back to its initial value (for tests): closes open packages
// (unmounts the roots), forgets the listeners and resets the language to English.
// Does not touch the content root.
void Reset();

}  // namespace kernel::xam
