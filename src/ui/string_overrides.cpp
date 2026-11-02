// Strings the game's own tables get wrong or lack, patched as LocString_GetString hands them out.

#include <mutex>
#include <string>
#include <unordered_map>

#include "core/localization.h"
#include "engine/dlc/dlc.h"
#include "ui/text_window.h"

using kameo::l10n::Tr;
using kameo::ui::GuestWideString;

// LocString_GetString(table, id) -- the save list's "A Select (B Back) Y Delete" hints (2142, 8142) gain the X Rename button ahead of Delete, and the downloadable skin names the vanilla table lacks come from the title update's list, as does the shader mod costume's.
extern "C" REX_FUNC(sub_8251DCE8) {
  const uint32_t id = ctx.r4.u32;
  __imp__sub_8251DCE8(ctx, base);
  // A missing id comes back as "<kameostringsStripped:ID Not Found>" rather than null.
  if (!ctx.r3.u32 || !REX_LOAD_U16(ctx.r3.u32) || REX_LOAD_U16(ctx.r3.u32) == u'<') {
    if (const std::u16string name = KameoDlcSkinName(id); !name.empty()) {
      if (const uint32_t address = GuestWideString(name)) ctx.r3.u64 = address;
      return;
    }
  }
  if ((id != 2142 && id != 8142) || !ctx.r3.u32) return;
  static std::mutex mutex;
  // Keyed by the game's text and our language, so switching language mid-game gets a fresh "Rename".
  static std::unordered_map<uint64_t, uint32_t> renamed;
  std::lock_guard<std::mutex> lock(mutex);
  const uint64_t key = uint64_t(ctx.r3.u32) | uint64_t(kameo::l10n::CurrentLanguage()) << 32;
  if (const auto found = renamed.find(key); found != renamed.end()) {
    ctx.r3.u64 = found->second;
    return;
  }
  std::u16string text;
  for (uint32_t i = 0; i < 256; ++i) {
    const char16_t c = char16_t(REX_LOAD_U16(ctx.r3.u32 + i * 2));
    if (!c) break;
    text.push_back(c);
  }
  const size_t delete_button = text.find(u"{button='Y'}");
  if (delete_button == std::u16string::npos) return;
  text.insert(delete_button, u"{button='X'} " + Tr(u"Rename") + u" ");
  if (const uint32_t address = GuestWideString(text)) {
    renamed.emplace(key, address);
    ctx.r3.u64 = address;
  }
}
