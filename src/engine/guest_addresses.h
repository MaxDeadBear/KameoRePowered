// Guest globals and guest functions shared by several engine hooks; names are the IDA names of the vanilla image.
#pragma once

#include <cstdint>
#include <cstring>

#include "core/guest.h"

namespace kameo::engine {

// player_list[4]: each entry is a player wrapper whose +0x10 is the actor and +0x200 the character object.
inline constexpr uint32_t kPlayerList = 0x8280D1A0;
inline constexpr uint32_t kNumPlayers = 0x82B718FC;
inline constexpr uint32_t kNumWantedPlayers = 0x8273A880;
inline constexpr uint32_t kIsSplitScreenMode = 0x82B70AF8;
inline constexpr uint32_t kCurrentSetupPoint = 0x827338D4;
// nextLevelPlayerObject is a char buffer, not a pointer: levelCreatePlayerSetup receives its address.
inline constexpr uint32_t kNextLevelPlayerObject = 0x82755BE8;
// playerControllers[36 * playerNo] holds the pad index; Sticks points at the pad records, 320 bytes each, +0 the held buttons and +8 those pressed this frame.
inline constexpr uint32_t kPlayerControllers = 0x82732F48;
inline constexpr uint32_t kSticks = 0x8276706C;
inline constexpr uint32_t kCommitTestButtonMask = 0x82731F54;
// The XLanguage id the string table and Bink language track are chosen from.
inline constexpr uint32_t kLangCodeGlobal = 0x827556B4;
// eventSetEx awards an event's score only while this is clear.
inline constexpr uint32_t kDoingTaskStart = 0x82B71CD8;

inline constexpr auto& ObjectAskObject = __imp__sub_82251DF8;
inline constexpr auto& ObjectWarpObjectPosAngle = __imp__sub_82252BE0;
inline constexpr auto& ObjectGetActiveObj = __imp__sub_82252530;
inline constexpr auto& ObjectIsPlayer = __imp__sub_82252588;
inline constexpr auto& ObjectKill = __imp__sub_82250C58;
inline constexpr auto& ObjectSetProcessPass = __imp__sub_82251260;
inline constexpr auto& ObjectSetRenderPass = __imp__sub_82251300;
inline constexpr auto& CameraGetActiveCamera = __imp__sub_822777F8;
inline constexpr auto& CameraSetupDefaultBlend = __imp__sub_82278740;
inline constexpr auto& CameraSetBlend = __imp__sub_82278918;
inline constexpr auto& CameraKillCameraBelongingTo = __imp__sub_822771E0;
inline constexpr auto& LevelCreatePlayerSetup = __imp__sub_8226D6A8;
inline constexpr auto& AudioStopObj = __imp__sub_822EABA0;
inline constexpr auto& KaiSackPopulateSack = __imp__sub_8217C958;
inline constexpr auto& EventDone = __imp__sub_822CC3C0;
inline constexpr auto& ObjPlayGetTrueHitPointPlayStatus = __imp__sub_8217CEC0;
inline constexpr auto& ObjectAlternativeSetFileInUse = __imp__sub_822502A0;
inline constexpr auto& AudioCategorySetVolumeString = __imp__sub_8230C050;
inline constexpr auto& BinkSetVolume = __imp__sub_826D21B0;
inline constexpr auto& LocStringCloseBlob = __imp__sub_8251E538;
inline constexpr auto& LocLoadStringTable = __imp__sub_820BF6E8;

// A scratch frame 0x200 below a guest stack pointer, so guest calls made from a hook push their frames below it.
inline uint32_t ScratchFrame(uint32_t stack_pointer) { return (stack_pointer - 0x200) & ~0xFu; }

inline float LoadGuestFloat(uint8_t* base, uint32_t address) {
  const uint32_t bits = REX_LOAD_U32(address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void StoreGuestFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  REX_STORE_U32(address, bits);
}

}  // namespace kameo::engine
