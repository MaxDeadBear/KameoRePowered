# Getting Started

This guide takes you from a fresh download to playing Kameo: RePowered, then covers the Xbox Guide, crash reports and what to do if something goes wrong.

Need a hand? Ask on the [Discord server](https://discord.gg/8jaZQUkxZZ).

## What you need

- **Your own copy of Kameo: Elements of Power for Xbox 360.** Kameo: RePowered contains no game data, so it needs your copy to run. This can be a disc image (`.iso`) or a Games on Demand package.
- **A 64-bit Windows or Linux PC** with a CPU that supports AVX2, which covers most from around 2013 onward, and a graphics card that supports Direct3D 12 or Vulkan.
- **About 6 GB of free disk space** for the game files, plus room for the download.

## Install on Windows

1. Go to the [releases page](https://github.com/MaxDeadBear/KameoRePowered/releases/latest) and download the file ending in `windows-x64.zip`.
2. Extract the whole zip into a new folder of its own, such as `C:\Games\Kameo`. Avoid `Program Files`, because the game saves its settings and logs in its own folder.
3. Open **KameoLauncher.exe**.
4. On the Play page, under **Set Up Your Game**, click **Choose Your Game...** and pick your `.iso`. For a Games on Demand package, switch the file type dropdown to **All files**.
5. Wait for the unpacking to finish. It usually takes a few minutes, and your original file isn't changed.
6. Press **Play**.

You only need to do this once. Your `.iso` isn't needed after that, so you can move it or keep it as a backup.

### The first launch

The first time you play, your PC prepares the game's shaders in the background while the game starts up. A panel in the bottom-left corner shows the progress ("Gathering the elements"), and you may see a little stutter until it finishes. Later starts are much quicker, and the panel usually won't appear at all.

### If Windows or your antivirus warns you

Kameo: RePowered isn't code-signed, so Windows SmartScreen may show "Windows protected your PC", and some antivirus programs may flag it. If you downloaded it from the official releases page, you can choose **More info**, then **Run anyway**. Never run a copy from anywhere else.

## Install on Linux

1. Download the file ending in `linux-x64.tar.gz` from the [releases page](https://github.com/MaxDeadBear/KameoRePowered/releases/latest) and extract it into a new folder.
2. Extract your game's files into a folder named `assets` beside `kameorepowered`, so that `assets/default.xex` exists. The launcher is Windows only, so this step is done by hand.
3. Run `./kameorepowered`.

## Updating

Open the launcher's **Builds** page to download and switch to a newer version. The Play page also tells you when one is out. Your settings, saves and game files are kept.

## Playing

Choose **Keyboard & Mouse** or **Gamepad** on the launcher's Play page. Controllers still work in Keyboard & Mouse mode. The launcher's other pages cover video, audio, controls, key bindings, mods and DLC, and most of these settings are also in the game's pause menu.

- **DLC:** install it from the launcher's DLC page.
- **Mods:** see [Making and installing mods](modding.md).

## The Xbox Guide (RexLiveless)

The Xbox Guide is a recreation of the Xbox 360's Guide, made by the RexLiveless project. It stands in for Xbox LIVE, which this game can no longer use.

**To open it,** press **F10** on the keyboard, or the **Guide button** on a controller (the Xbox button, or Home on a Switch Pro Controller). Move around with the D-pad or left stick, press **A** to select and **B** to go back. The game ignores your controls while the Guide is open.

**The first time you start the game,** the Guide asks you to create a profile or sign in. If you'd rather skip that, choose **Play Offline**.

**With a profile, you get:**

- A gamertag, gamerpics and a profile photo
- Friends, friend requests and messages
- Achievements, with unlock pop-ups
- Game invites, online sessions and System Link
- Themes, including a theme designer

Parties and voice chat aren't available. Online play is new in this game and is still being tested, so tell us on Discord how it goes.

**About your data:** profiles live on [rexliveless.xyz](https://rexliveless.xyz), a service run by the RexLiveless project rather than by Kameo: RePowered. Use a password you don't use anywhere else. Your saved sign-in is kept in the `guide` folder beside the game, so don't share that folder.

**To turn the Guide off,** add this line to `kameorepowered.toml` beside the game, then restart:

```toml
kameo_rexliveless = false
```

While the Guide is on, the game connects to rexliveless.xyz when it starts, even if you never sign in, to fetch the lists of gamerpics and themes. With it off, there's no Guide, no sign-in screen and no connection to that server.

### Cheats and leaderboards

Cheated scores stay off the leaderboards. This covers **Unlock All Extras** and **All Warriors & Upgrades**. Level Select and the collectible tracker don't count as cheats.

The first time a cheat takes effect, the game backs up your saves. Leaderboard scores then stay off, even if you turn the cheat off again.

If you turned one on by accident, close the game, open the launcher's **Mods** page and choose **Restore Saves From Before Cheats**. That puts your saves back, turns the cheats off and lets you post scores again. Any progress since the backup goes back too, but your newer saves are kept in `save_backups` inside `Documents\kameorepowered`.

## Crash reports

Crash reports are **completely optional**. Most crashes never get reported on Discord or GitHub, so a report is how the developers find out what went wrong and fix it.

On Windows, if the game crashes, a window asks whether to send a report:

- **Send Report** sends it to the developers through [Sentry](https://sentry.io).
- **Don't Send**, Escape or closing the window sends nothing.

The buttons wait two seconds before they respond, so a click meant for the game can't send a report by accident.

**A report contains:** the game's log from that session, the crash's call stacks, the game and OS versions, and a random ID for your install. It can show file paths, including your Windows user name if the game is in your user folder. The call stacks hold small pieces of the game's memory, which could rarely include your RexLiveless sign-in if it crashes while using it. Your IP address and location aren't stored.

**To stop being asked,** turn off **Offer to Send Crash Reports** on the launcher's Play page. Nothing is sent to Sentry at any other time, and Linux builds never send reports.

The game also writes its own crash files to the `logs` folder, and these stay on your PC.

## Troubleshooting

| What you see | What to do |
| --- | --- |
| **"The game files are missing"** | Choose your game on the launcher's Play page first. |
| **"could not be read"** or **"is incomplete or damaged"** | The file is probably an unfinished download or a bad copy. Make a fresh copy and try again. |
| **"is a different game"** | That image isn't Kameo: Elements of Power. |
| **"Not enough free space"** | Free up the amount shown on that drive, or put the game on another drive. |
| **"The assets folder already has files in it"** | Move those files out of the `assets` folder, then try again. |
| **The game unpacked but won't start, or crashes right away** | Your copy may be a different version from the one this build expects. Ask on Discord and include your log. |
| **A dark screen with nothing on it** | If the corner panel is moving, it's still working. If there's no panel and nothing changes for a few minutes, close the game and ask on Discord. |

### Reporting a bug

Open a [GitHub issue](https://github.com/MaxDeadBear/KameoRePowered/issues) or ask on Discord, and attach these from the `logs` folder:

- the newest `kameorepowered_*.log`
- the `crash_*.txt` file, if the game crashed

**Don't post a `crash_*.dmp` file publicly.** Dump files can hold your sign-in details.

## Removing the game

Delete the game's folder. That also removes your saved Guide sign-in. Your saves and DLC are stored separately in `Documents\kameorepowered`, so delete that folder too if you don't want to keep them.
