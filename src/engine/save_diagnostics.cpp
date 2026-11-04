// Logging for the Select game page that sometimes offers only "Start without saving": the page lists the saves only
// when XamContentGetDeviceState accepts the storage device the game chose (0x8275C25C), which is -1 from the moment
// the device selector is asked until it answers, and the sign-in handler can drop the choice too. Each line gives the
// order those happened in.

#include <cstdint>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>

#include "core/guest.h"
#include "kameorepowered_init.h"

namespace {

constexpr uint32_t kSaveDevice = 0x8275C25C;      // the storage device chosen, -1 for none
constexpr uint32_t kSelectorResult = 0x8275C224;  // the device selector's overlapped block: 997 while it runs
constexpr uint32_t kNoSaving = 0x8275C260;        // set while playing without saving
constexpr uint32_t kNotifyPrompt = 0x82B70AC4;    // the prompt systemNotify shows: 1 signed out, 2 back in, 3 device gone, 4 none
constexpr uint32_t kSigninSeen = 0x82759D28;      // the sign-in state the notify handler last saw

std::string State(uint8_t* base) {
  return fmt::format("device {} selector {} without-saving {} prompt {} sign-in seen {}", int32_t(REX_LOAD_U32(kSaveDevice)),
                     REX_LOAD_U32(kSelectorResult), REX_LOAD_U8(kNoSaving), REX_LOAD_U32(kNotifyPrompt), REX_LOAD_U32(kSigninSeen));
}

}  // namespace

// The Select game page's text (IDA: xfixgem_save_promptCorruptSaveNotification): only "Start without saving" when the
// device check fails, else New Game, Start without saving and the saves.
extern "C" REX_FUNC(sub_823EDE40) {
  const uint32_t record = ctx.r3.u32 ? REX_LOAD_U32(ctx.r3.u32 + 512) : 0;
  REXLOG_INFO("[kameo] saves: Select game page built, screen state {}, {}", record ? int32_t(REX_LOAD_U32(record + 24)) : -1, State(base));
  __imp__sub_823EDE40(ctx, base);
}

// xfixgem_save_checkStorageDeviceValid(show) -- sets the device to -1 and asks the device selector, or returns -1 when
// nobody is signed in.
extern "C" REX_FUNC(sub_820BD518) {
  const uint32_t show = ctx.r3.u32;
  __imp__sub_820BD518(ctx, base);
  REXLOG_INFO("[kameo] saves: device selector asked ({}) -> {}, {}", show, int32_t(ctx.r3.u32), State(base));
}

// xfixgem_systemNotify_handleSigninStateChange -- on a sign-in change it can mark the game as playing without saving,
// or raise the "logged back in" prompt.
extern "C" REX_FUNC(sub_820B2968) {
  const std::string before = State(base);
  __imp__sub_820B2968(ctx, base);
  REXLOG_INFO("[kameo] saves: sign-in changed: before {}; after {}", before, State(base));
}
