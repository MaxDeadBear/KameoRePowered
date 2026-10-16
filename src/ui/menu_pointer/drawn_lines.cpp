// Records where the game really draws each menu line, so the pointer lands on what the player sees.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "gpu/draw_path.h"
#include "ui/hud/fps_counter.h"
#include "ui/menu_pointer/menu_ui_state.h"
#include "ui/text_window.h"

using namespace kameo::ui;

namespace {

// Collects one window's lines on the render thread while xfix_textWindow_render runs, and publishes them when it returns.
class DrawnLinesPass {
 public:
  explicit DrawnLinesPass(uint32_t text_object) : previous_(t_current) {
    text_object_ = text_object;
    t_current = this;
  }
  ~DrawnLinesPass() {
    t_current = previous_;
    if (!text_object_ || lines_.empty()) return;
    StoreDrawnLines(text_object_, std::move(lines_));
  }

  static void NoteLine(uint8_t* base, uint32_t text_object, uint32_t item) {
    DrawnLinesPass* pass = t_current;
    // xfix_textWindow_render draws from the render thread's buffered copy of the window, so the text object here is that copy rather than the one the pass was opened for.
    if (!pass || !text_object || !item) return;
    pass->font_ = REX_LOAD_U32(text_object + kTextFont);
    if (REX_LOAD_U32(item + kItemSelectable) != 1) {
      pass->item_index_ = -1;
      return;
    }
    pass->item_index_ = pass->selectables_++;
    pass->item_height_ = LoadFloat(base, item + kItemHeight);
    pass->item_printed_ = false;
  }

  static void NotePrint(uint8_t* base, uint32_t font) {
    DrawnLinesPass* pass = t_current;
    if (!pass || font != pass->font_ || pass->item_index_ < 0 || pass->item_printed_) return;
    pass->item_printed_ = true;
    const float top = LoadFloat(base, font + 952);
    pass->lines_.push_back({pass->item_index_, top, top + pass->item_height_});
  }

 private:
  static thread_local DrawnLinesPass* t_current;
  DrawnLinesPass* previous_;
  uint32_t text_object_ = 0;
  uint32_t font_ = 0;
  int32_t selectables_ = 0;
  int32_t item_index_ = -1;
  float item_height_ = 0.0f;
  bool item_printed_ = false;
  std::vector<DrawnLine> lines_;
};

thread_local DrawnLinesPass* DrawnLinesPass::t_current = nullptr;

}  // namespace

// A size tag draws a line at 0.943 of its size whatever height it was measured at, so the ratio is the scale.
float kameo::ui::WindowTextScale(uint8_t* base, uint32_t text_object) {
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) {
    if (REX_LOAD_U32(item + kItemSelectable) != 1) continue;
    const uint32_t lines = std::max<uint32_t>(1, REX_LOAD_U8(item + kItemLineCount));
    const float line_height = LoadFloat(base, item + kItemHeight) / float(lines);
    std::u16string text;
    // item+12 is the line as formatted, so a size tag from the format string counts as much as one in the text.
    for (uint32_t i = 0, address = REX_LOAD_U32(item + kItemFormattedText); address && i < 256; ++i) {
      const char16_t c = char16_t(REX_LOAD_U16(address + i * 2));
      if (!c) break;
      text.push_back(c);
    }
    const size_t tag = text.find(u"size='");
    if (tag == std::u16string::npos || line_height <= 0.0f) return 1.0f;
    int32_t size = 0;
    for (size_t at = tag + 6; at < text.size() && text[at] >= u'0' && text[at] <= u'9'; ++at) size = size * 10 + (text[at] - u'0');
    return std::clamp(std::round(float(size) * 0.943f / line_height), 1.0f, 4.0f);
  }
  return 1.0f;
}

// xfix_textWindow_render(matrix, text) -- marks the FPS counter's own draw, so its box is drawn without the drop shadow.
extern "C" REX_FUNC(sub_82108D08) {
  MarkFpsCounterRender(IsFpsCounterText(base, ctx.r3.u32));
  // StoryBookMainObj_ask points the text target size at 640 (away from its -1 sentinel at 0x82733100) only while it draws windows into the page texture.
  if (ctx.r3.u32 && LoadFloat(base, 0x8274D524) != LoadFloat(base, 0x82733100)) NoteBookPageWindow(ctx.r3.u32);
  DrawnLinesPass pass(ctx.r3.u32);
  const uint32_t outer_group = kameo::gpu::g_hudGroup;
  if (!outer_group) kameo::gpu::g_hudGroup = ctx.r3.u32;
  __imp__sub_82108D08(ctx, base);
  kameo::gpu::g_hudGroup = outer_group;
  MarkFpsCounterRender(false);
}

// textSetFontPropertiesFromStyle(textObject, style) -- the render loop calls it once per line, printed or clipped, with the line's copied style at item+24, so it numbers the selectable lines the way menu+114 counts them.
extern "C" REX_FUNC(sub_8210B008) {
  DrawnLinesPass::NoteLine(base, ctx.r3.u32, ctx.r4.u32 ? ctx.r4.u32 - 24 : 0);
  __imp__sub_8210B008(ctx, base);
}

// fontPrintSubstring(font, text, ...) -- the first print of a selectable line records where it really went: the pen's y at font+952 is the line's top after centring, scrolling and overwriting.
extern "C" REX_FUNC(sub_820FE390) {
  DrawnLinesPass::NotePrint(base, ctx.r3.u32);
  __imp__sub_820FE390(ctx, base);
}
