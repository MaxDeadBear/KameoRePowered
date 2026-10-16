// The skin hot-swap queue between the UI thread, which picks a skin, and the game tick, which loads it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern std::atomic<int> g_kameo_pending_dlc_swap;
extern std::atomic<int> g_kameo_active_dlc_swap;
extern std::atomic<int> g_kameo_dlc_swap_enabled;
extern std::atomic<int> g_kameo_pending_standard_swap;
extern std::atomic<int> g_kameo_stop_next_dlc_model_load;
extern std::atomic<int> g_kameo_pending_dlc_suffix_ready;
extern std::atomic<int> g_kameo_active_dlc_suffix_ready;

void ClearKameoDlcSuffix();
void QueueKameoDlcSuffix(std::string_view suffix, bool make_active = true);
bool ConsumeKameoPendingDlcSuffix(char* out, size_t out_size);
bool ReadKameoActiveDlcSuffix(char* out, size_t out_size);
