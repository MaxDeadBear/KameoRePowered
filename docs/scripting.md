# Script mods

A script mod changes how the game behaves with Lua: it reacts to what the game does, reads and writes the game's settings and memory, hooks the game's functions, and draws on screen. It needs no build tools, only a text editor.

Script mods run on [RexFabric](https://github.com/masterspike52/RexFabric), the modding framework for ReXGlue games. Every mod gets two Lua tables:

- `kameo` is this game: levels, players, costumes and the game's settings. It is described on this page.
- `rex` is RexFabric: keys and the mouse, timers, events, per-mod settings, text and drawing on screen, hooks on the game's functions, calling them, and the game's memory.

Models, levels, textures and shaders are made with the other tools in [Mods](modding.md); a script mod can sit beside them.

## Your first mod

1. Open the `mods` folder beside `kameorepowered.exe`. Make it if it is not there.
2. Copy [`docs/examples/mods/hello_kameo`](examples/mods/hello_kameo) into it, so that you have:

   ```
   mods/
     hello_kameo/
       mod.toml
       scripts/
         main.lua
   ```

3. Start the game. Press **F5** for the mods menu, where the mod is listed and can be switched off, reordered and reloaded.

The folder's name is the mod's id, so use lower case letters, digits and `_`, and keep it once chosen. `mod.toml` is what the menu shows; every line is optional:

```toml
name = "Hello Kameo"
description = "Shows the level's name when it changes."
version = "1.0"
author = "You"
```

Every `.lua` file in `scripts` runs when the game starts, after the game is loaded and before it runs. `print` goes to the game's log (`logs/kameorepowered_<n>.log`), with the mod's folder in front, and so does any error with its file and line. **Reload Scripts** in the mods menu runs them again without restarting.

```lua
print("Hello from " .. kameo.build())

kameo.on("level_changed", function(name)
  rex.screen_text("Level: " .. name, 4, 0xFFD070)
end)

rex.on("frame", function()
  local pressed = rex.input.pressed("N")       -- every frame, so no press is missed
  if pressed and rex.input.accepts() then      -- not while a menu has the keyboard
    kameo.setting.set("kameo.camera_fov", "10")
  end
end)
```

## Keys

| Key | What |
| --- | --- |
| **F5** | The mods menu: switch mods on and off, order them, reload scripts, open the folder. It is also a page of the Guide (F10). |
| **F2** | The node editor, which makes a mod's Lua from a graph of nodes. |

Both can be rebound as `ui.bind.mods` and `ui.bind.node_editor`.

## The `kameo` table

Every function can be called at any time. What has to happen on the game's own thread (a level change) is done at the game's next tick.

| Function | What |
| --- | --- |
| `kameo.build()` | The build's name, such as `Kameo: RePowered 0.3.5`. |
| `kameo.title_update` | `true` in the title-update build (`kameorepowered-tu`), `false` in the original. |
| `kameo.level.name()` | The level the game is in, by the name the game uses: `FrontEndLevel`, `CastleIntro`. Empty before the first level. |
| `kameo.level.change(name, setup_point)` | Fades out and loads a level by name, with the player at that numbered setup point (`0` for its first). |
| `kameo.level.start_task(task)` | Starts a story task by number, as the pause menu's level select does. It resets the story's events to that task, so keep it away from a save you care about. |
| `kameo.player.count()` | How many players are in the game. |
| `kameo.player.object(n)` | The address of player `n`'s character (`0` to `3`), for `rex.mem` and `rex.call`; `0` when there is none. |
| `kameo.player.x(n)`, `.y(n)`, `.z(n)`, `.yaw(n)` | Where player `n` is in the level (`y` is up) and which way they face, in radians. `0` when that player is not in the game. |
| `kameo.player.set_position(n, x, y, z)` | Puts player `n` somewhere else in the level. Nothing checks that the place can be stood in. |
| `kameo.player.set_run_speed(factor)`, `kameo.player.run_speed()` | Player one's pace along the ground as a multiple of the game's own: `1` is unchanged, `2` twice as fast. The extra distance is added after the game's own movement and is not checked against walls, so a large factor can pass through thin ones. |
| `kameo.cheat.infinite_health(on)`, `.infinite_energy(on)`, `.one_shot(on)`, `.super_jump(on)` | The switches behind the game's cheats, `true` or `false`. |
| `kameo.score.add(points)` | Adds to the score. |
| `kameo.costume.set(suffix)` | Puts Kameo in a costume by its file suffix (`"00"` is Classic). An empty string puts the standard look back. |
| `kameo.setting.get(name)` | One of the game's settings as text: `"true"`, `"10"`. Names are those in `kameo_settings.toml`, such as `kameo.camera_fov` or `kameo.gfx_bloom`. |
| `kameo.setting.number(name)`, `kameo.setting.boolean(name)` | The same, as a number or a boolean. |
| `kameo.setting.exists(name)` | Whether the game has that setting. |
| `kameo.setting.set(name, value)` | Changes a setting from text and saves it; `false` when the setting is missing or refuses the value. Switching on a cheat this way counts as using it: saves are backed up and scores stay off the leaderboards, as from the pause menu. |
| `kameo.on(event, fn)` | Runs `fn` whenever the game fires that event. |

| Event | Arguments | When |
| --- | --- | --- |
| `level_changed` | the level's name | The game has started changing to another level. |

RexFabric's own `frame` event (`rex.on("frame", function(seconds) ... end)`) fires once per game tick, on the game's thread.

## Mods and the leaderboards

A mod can change anything in the game, so scores are not posted to the leaderboards while any mod in the `mods` folder is switched on. Nothing is marked in your saves: switch the mods off in the mods menu and restart, and scores post again.

A second example, [`speed_keys`](examples/mods/speed_keys), changes how fast Kameo runs with the numpad's `+`, `-` and `*`.

## The `rex` table

`rex` is the same in every RexFabric game: `rex.on`, `rex.after` and `rex.every`, `rex.input`, `rex.cvar` for a mod's own settings, `rex.screen_text` and `rex.draw`, `rex.hook` and `rex.hook_after`, `rex.call`, `rex.mem`. Its reference is RexFabric's, written for the first game that used it: [The RexFabric API](https://github.com/masterspike52/reNut/blob/main/docs/rexfabric-api.md) and [Mods and scripts](https://github.com/masterspike52/reNut/blob/main/docs/mods-and-scripts.md). Where those pages say `renut`, read `kameo`.

## Hooks and addresses

`rex.hook(address, fn)` runs `fn` before one of the game's functions, and `rex.hook_after` after it. An address is the function's place in the game's executable, the number in the names this project uses for them (`sub_8224F700` is `0x8224F700`).

- **The two builds have different addresses.** The title update moved most of the game's code, so a mod that hooks by address works in one build only. Check `kameo.title_update` and carry both addresses, or keep to the `kameo` functions, which are the same in both.
- **Functions this project replaces cannot be hooked yet.** Where our own code takes a function over (the renderer's entry points, the pause menu, the fixes in `src/engine`), a mod's hook on the same address does not run.
