// Title-update DLC packages on the vanilla image: their skin lists are cut down to what vanilla understands, and every package's list gets read.

#include <algorithm>
#include <string>

#include <rex/logging.h>

#include "core/guest.h"

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
