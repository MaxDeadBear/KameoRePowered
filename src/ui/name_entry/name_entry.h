// The in-game name entry that replaces the system keyboard.
#pragma once

// True while it is open; keys typed on a physical keyboard are queued into it (8 backspace, 13 enter, 27 escape).
bool KameoNameEntryActive();
void KameoNameEntryPushKey(char32_t key);
