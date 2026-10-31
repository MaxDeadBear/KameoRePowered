// What KameoCrashDialog.exe and the game's crash reporter agree on.
#pragma once

// The dialog's exit code when the player chooses to send. Anything else, such as the dialog itself failing, means no.
constexpr unsigned long kCrashDialogSendReport = 0x53454E44;
