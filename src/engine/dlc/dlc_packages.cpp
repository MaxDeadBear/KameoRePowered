// Title-update DLC packages on the vanilla image: their skin lists are cut down to what vanilla understands, and every package's list gets read. On the title update's own image, its pack listener doesn't remount everything at boot.

#include <algorithm>
#include <string>

#include <rex/logging.h>

#include "core/guest.h"
#include "kameorepowered_init.h"

// The first two fixes are for the vanilla image: the title update reads its own packs whole.
#if !KAMEO_TU

// xfixgem_dlc_enumeratePackagesAndValidate(async, _, package) -- parses the KameoDLCList.txt just read into byte_827588D4, registering each line up to its "\r\n" as a model skin and abandoning the list at the first line of 43 characters or more. Title-update packs open with a KameoDownloadableContentVersion line and carry KameoDownloadableModelLicense group lines, string tables and levels, so vanilla dropped them whole; the list is cut down to its .mdl lines, each ending in "\r\n", before the game reads it.
extern "C" REX_FUNC(sub_820B0B90) {
  constexpr uint32_t kListBuffer = 0x827588D4;
  constexpr uint32_t kListBufferSize = 5000;
  std::string list;
  for (uint32_t i = 0; i < kListBufferSize; ++i) {
    const char c = char(REX_LOAD_U8(kListBuffer + i));
    if (!c) break;
    list.push_back(c);
  }
  std::string models;
  size_t start = 0;
  while (start < list.size()) {
    size_t end = list.find('\n', start);
    if (end == std::string::npos) end = list.size();
    std::string line = list.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const bool model = line.size() > 4 && line.size() < 42 && (line.compare(line.size() - 4, 4, ".mdl") == 0 || line.compare(line.size() - 4, 4, ".MDL") == 0);
    if (model && models.size() + line.size() + 2 < kListBufferSize) models += line + "\r\n";
    start = end + 1;
  }
  if (models != list) {
    for (uint32_t i = 0; i < kListBufferSize; ++i) REX_STORE_U8(kListBuffer + i, i < models.size() ? uint8_t(models[i]) : 0);
  }
  std::string package_name;
  for (uint32_t i = 0, name = ctx.r5.u32 ? REX_LOAD_U32(ctx.r5.u32 + 36) : 0; name && i < 42; ++i) {
    const char c = char(REX_LOAD_U8(name + i));
    if (!c) break;
    package_name.push_back(c);
  }
  REXLOG_INFO("[kameo] dlc {}: {} skin models", package_name, std::count(models.begin(), models.end(), '\n'));
  __imp__sub_820B0B90(ctx, base);
}

// downloadableClosed(error, extended, data) -- when a package's list has been read it calls the queued continuation (dword_82759C6C, _downloadableLoadMoreDLCListFile) and only then clears it, but that continuation has already queued itself again for the next package, so the clear dropped it and vanilla never read past the second pack; the slot is cleared before the call instead.
extern "C" REX_FUNC(sub_820B0CA8) {
  constexpr uint32_t kContinuation = 0x82759C6C;
  constexpr uint32_t kContinuationData = 0x82759C70;
  constexpr uint32_t kDownloadableSystem = 0x827588D0;
  const uint32_t continuation = REX_LOAD_U32(kContinuation);
  if (!continuation) {
    __imp__sub_820B0CA8(ctx, base);
    return;
  }
  REX_STORE_U32(kContinuation, 0);
  REX_STORE_U32(kDownloadableSystem, 0);
  PPCContext call = ctx;
  call.r3.u64 = REX_LOAD_U32(kContinuationData);
  {
    PPCContext& ctx = call;
    REX_CALL_INDIRECT_FUNC(continuation);
  }
}

#else

// The title update's downloadable-content listener (new in the update, so no original twin) mounts every pack again when XN_SYS_SIGNINCHANGED shows the primary player going from signed out to signed in. Its last-seen sign-in state (dword_82BCFE94) starts at 0, so the Guide's first sign-in notice, which lands about a second into boot while the packs are mounting, always looked like a fresh sign-in: every pack was closed and mounted again under new dlcN names while a sound bank was still loading from dlc2:, and that read failing put up the dirty-disc error. A console sends no such notice for a profile already signed in when the game starts, so the state is seeded with the player's real one the first time the listener runs, and only a real sign-in mounts the packs again.
extern "C" REX_FUNC(sub_820B0878) {
  constexpr uint32_t kListener = 0x827B3790;
  constexpr uint32_t kPrimaryPad = 0x82784510;
  constexpr uint32_t kSigninState = 0x82BCFE94;
  static bool seeded = false;
  if (!seeded && REX_LOAD_U32(kListener) != 0) {
    seeded = true;
    if (REX_LOAD_U32(kSigninState) == 0) {
      PPCContext call = ctx;
      call.r3.u64 = REX_LOAD_U8(kPrimaryPad);
      sub_82554030(call, base);  // XamUserGetSigninState
      REX_STORE_U32(kSigninState, call.r3.u32);
      REXLOG_INFO("[kameo] dlc: the listener starts with sign-in state {}", call.r3.u32);
    }
  }
  __imp__sub_820B0878(ctx, base);
}

// The title update's KameoDLCList.txt parser (new in the update; (async, size, package)). In a version-0 pack (the
// Fright Pack and Masquerade bundle) each model belongs to the "KameoDownloadableModelLicense>>>…n" group above it, and
// counts as owned only when bit n is set in the pack's licence mask (+76), which opening the pack fills from its header.
// A console wrote the bits bought into that header; a pack installed here has none, so the bundle's skins never showed.
// Having the pack's files is taken as owning all of it.
extern "C" REX_FUNC(sub_820B1268) {
  const uint32_t package = ctx.r5.u32;
  if (package && REX_LOAD_U32(package + 76) == 0) {
    REX_STORE_U32(package + 76, 0xFFFFFFFFu);
    REXLOG_INFO("[kameo] dlc: a pack with no licence bits counts as owned in full");
  }
  __imp__sub_820B1268(ctx, base);
}

#endif  // !KAMEO_TU
