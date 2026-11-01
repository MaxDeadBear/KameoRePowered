# Mods

Kameo: RePowered loads two kinds of mod:

- **`.kmod` packages** change level archives (`.lvl`) and character models (`.mdl`): a new skin for one of Kameo's
  costumes, a hat, a recoloured warrior. A `.kmod` holds only the differences from the game's own files, so it carries
  none of the game's data and can be shared freely.
- **Shader mods** (`.kshader`) change how things are drawn: cel shading, a rim of light, comic dots. See
  [Shader mods](#shader-mods).

Mods need the Windows launcher, `KameoLauncher.exe`, beside the game.

## Installing a mod

1. Open the launcher and go to the **Mods** page.
2. Click **Open Folder** and copy the `.kmod` file into the folder that opens.
3. Click **Refresh**. The mod appears in the list; switch it on.
4. Click **Play**.

When you press Play, the launcher builds a copy of the game folder with your mods applied (`mods_build`, made of
links, so it is quick and small) and starts the game from it. The game's own files are never changed. To remove a
mod, switch it off, or delete the file.

- **Close the game before changing mods.** The launcher won't rebuild while the game is still running from the
  modded copy; it says so and leaves it as it is.
- **Order matters only when two mods change the same file.** The launcher warns you, and the mod lowest in the list
  wins. Use the arrows to reorder.

### Kameo skins and the Bonus costumes

A Kameo skin replaces one of her three Bonus costumes, not the green outfit she starts in. To see it:

1. Unlock the costumes. They are earned in the story, or unlock every extra at once: on any pause or options page,
   press **X, X, Y, Y, LB, LB, RB, RB** on a controller, then turn on **Unlock All Extras** (pause menu
   Gameplay page, or the launcher's Mods page).
2. In the storybook's **Bonus** pages, choose the costume the mod replaces: on the Kameo costume line, left and right
   step through her costumes, as does A.

A skin may replace one costume or all three; the launcher lists the files each mod changes (`007183BF_00.mdl`,
`_01` and `_02` are the Classic, Gothic and Coyote costumes). A [shader mod](#shader-mods) for her everyday outfit adds
a costume instead of replacing one.

## Making a mod with Blender

The Windows download includes a Blender add-on in its `modkit` folder:

| File | For |
| --- | --- |
| `kameo_modkit.zip` | Blender 3.6 or newer |
| `kameo_modkit_blender279.zip` | Blender 2.79 (also needs a newer Blender, see below) |

### Setting it up

1. In Blender: **Edit > Preferences > Add-ons > Install...**, pick `kameo_modkit.zip`, and tick **Kameo Modkit**.
2. In the add-on's settings, set **Game folder** to the folder you installed Kameo: RePowered in (the one holding
   `KameoLauncher.exe`).

### Editing a character

1. **File > Import > Kameo Character**, and pick one by name: one of Kameo's costumes, or a warrior's skin. It opens
   rigged and textured.
2. Change it however you like: reshape it, delete parts, repaint its textures.
3. To add something (a hat, armour, a whole new body), import or model it, place it on the character, select it, and
   in the 3D view's sidebar (press **N**) open the **Kameo** tab and click **Attach to Character**.
4. **File > Export > Kameo Mod**. Give it a name; it goes straight into the launcher's mods folder, switched on.

If something can't be exported, the export explains why. Its report also lists what it changed for you, such as
parts split up or textures resized: read it when something looks wrong in game.

### How attached parts move

Attach offers four choices (in the operator panel, bottom left, after clicking it):

- **Automatic**: keeps the part's own weights if it is weighted to Kameo's `joint_` bones; otherwise big parts bend
  with the body and small ones follow the nearest bone.
- **Bend with the body**: copies Kameo's skin weights from the nearest part of her. For clothes, hair, or a whole new
  body.
- **Follow one bone**: moves rigidly with one bone. For hats, props, armour plates.
- **Keep its weights**: for a model you rigged to her `joint_` bones yourself.

A part that follows more bones than one draw can hold is split automatically.

### Replacing the whole character

1. Import the character and delete her meshes (keep the armature).
2. Bring your model in, then scale, turn and move it so it stands where she stood, in the same T-pose. Kameo is about
   5.6 units tall and faces −Y (Blender's Front view). Models made for other games are often far smaller and facing
   another way.
3. Select every part and click **Attach to Character** (Automatic, or Bend with the body).
4. Export.

Things to expect:

- **Her face won't animate.** Blinking and mouth shapes belong to her own face; a new face follows her head and eyes.
- **Clipping.** Parts follow her animations, so check legs poking through skirts and the like.
- **The skin replaces one costume file.** Make one mod per costume (import each costume in turn) to cover all three.

### Textures

- **Each material shows one image**, as its Base Color, and a normal map if you give it one. Parts without an image
  are coloured with their material's colour.
- **Images must load.** A texture whose file is missing (a path on someone else's drive, an image painted and never
  saved) becomes a flat colour. Use **File > External Data > Pack Resources** (Pack All into .blend in 2.79) before
  you share a model, and **Image > Save** painted images.
- **Layered textures don't carry over.** If a material mixes several images (Blender 2.79's texture stack, or a node
  setup), bake it into one image first.
- **No transparency or shine on new parts.** New parts copy the shading of an ordinary part of the character, so
  eyelashes and glass show solid.
- **UVs.** Images repeat, so UVs outside 0–1 are fine. They are stored in a limited range; the export moves them by
  whole tiles to fit, and says so.

### Rules underneath

These matter if you edit the exported files by hand or something looks off:

- **Materials name draws.** Each of the character's materials is one of the game's draws,
  `graph<g>_mesh<m>_draw<d>`; triangles go wherever their material says, so objects can be joined or split freely.
  A copy of one (`….001`) becomes a new draw with its own textures and bones. A draw left with no triangles is
  hidden.
- **The skeleton is the game's.** Weight to the existing `joint_NN` bones. Bones can't be added, and moving them
  changes nothing in game.
- **Bone budget.** Each draw holds up to 12 bones; parts using more are split into extra draws.
- **Limits.** 65535 vertices per mesh. A mesh can't reach further from the character than the original did.

### Blender 2.79

Install `kameo_modkit_blender279.zip` instead (**File > User Preferences > Add-ons > Install from File**). Blender
2.79 can't run the modkit itself, so it hands the work to a newer Blender in the background:

1. Download a portable Blender 2.93 or newer from blender.org and unzip it anywhere. You never have to open it.
2. In the add-on's settings, set **Game folder** and **Newer Blender** (that Blender's `blender.exe`).
3. **File > Import > Kameo Character** and **File > Export > Kameo Mod** work as above.

There is no Attach button in 2.79: the export attaches everything in the scene that isn't Kameo's. Parts weighted to
her `joint_` bones keep those weights; the rest take hers. Keep her bone names and material names as they are.

## Shader mods

A shader mod changes how the game draws things, without touching any model or texture. Make one with
`KameoShaderEditor.exe` beside the game (the **Shader Editor** button on the launcher's Mods page), from a node graph or
in HLSL.

### Making one

1. Name your mod at the top left, and pick a look from **Start from...** or build your own graph.
2. Choose what it changes under **Applies to**.
3. Watch the preview. Pick a character and any of their costumes above it: drag to turn, right drag to move, wheel to
   zoom, double-click to frame. Installed DLC skins are listed too.
4. Run the game alongside it. With **Live Update** on, the game picks up each change a moment after you stop editing,
   with no restart.
5. Press **Save**. It compiles for both renderers and keeps your graph, so **Open...** can bring it back.

The graph works like Unreal's material editor: drag nodes in from the palette, or right-click the canvas to search for
one. Everything ends in **Output**, the colour drawn. Hover a node for what it does. Name a Number or Colour node to
list it under Parameters, where it's quick to tune.

The nodes to start with:

| Node | Gives |
| --- | --- |
| Game colour | The pixel as the game lit it. Wire it straight to Output to change nothing |
| Base texture | The surface's colour before lighting |
| Light | The light the game worked out, with its shadows |
| Key light | A light placed from the camera, which looks the same in every scene |
| Sun light | The level's own sun or main light |
| Rim | 0 facing the camera, 1 at the silhouette |
| Outline | 1 on an inked line a set number of pixels wide just inside the silhouette, and round any part seen in front of another |

### What to expect

- **Applies to chooses shaders, not objects.** Anything else the game draws with the same shaders changes too, so a
  mod for one warrior can reach another that shares its look, and a mod for Kameo's costumes can reach another costume.
  **Kameo: new costume (everyday outfit)** is the exception: chosen alone, it holds to that outfit exactly, and makes a
  costume of its own (see below). A wider choice takes longer to compile: Everything in the game takes a few minutes,
  and Live Update stops above 400 shaders, so press Save instead.
- **A mod for her everyday outfit is a new costume, not a change to that outfit.** It's listed after the last of her
  costumes on the Bonus pages, under the mod's file name (so `Cel Shaded Kameo.kshader` reads like the others). On the
  Kameo costume line, left on the stick or d-pad steps back a costume, and right or A steps forward, so from her
  everyday outfit one press left reaches the last mod. Each such mod is a costume of its own, in file name order, so
  you can keep several versions of a look side by side. Her everyday outfit stays as it was, and your save remembers
  which you chose. The costume line needs one of her costumes unlocked (see
  [Kameo skins and the Bonus costumes](#kameo-skins-and-the-bonus-costumes)). With the mod switched off, the
  costume leaves the list and she wears her everyday outfit.
- **Reading Base texture or Light costs more.** To work them out, each shader runs a second time with its lighting
  turned off, so a mod that uses them draws those surfaces at twice the work. A mod that doesn't use them pays nothing.
- **Base texture is often paler than a surface looks in game,** because the game's lights darken and warm it. A comic
  look can use that: `Cel Shaded Kameo` keeps her pale and draws one hard shadow shape on her, and its Scene colour
  setting brings in as much of a level's colour as you want.
- **The game lights in linear light and brightens the picture at the very end.** Numbers look brighter on screen than
  they are: 0.5 shows at about 73%, so a shadow colour needs to be far darker than it looks like it should be.
- **Light is worked out from the other two,** so it holds shine as well as light.
- **Key light follows the camera,** which keeps the shadow on the same side of a character wherever they turn.
- **Surfaces the game doesn't light are left alone:** glows, sparks and other effects.
- **Outlines are drawn on the surface, just inside its edge,** since a shader can only colour its own pixels. They
  keep their width in pixels at any distance. Strongly bumpy surfaces can show specks of line; lower the width or leave
  Outline off there.
- **The preview's lighting stands in for a level's.** Run the game to see the real thing.
- **Mods can't read pictures of their own yet.**

### Installing and sharing

Shader mods live in the `shader_mods` folder beside the game. To install one, put its `.kshader` file there (**Open
Folder** on the launcher's Mods page). Switch mods on and off on that page; it takes effect at once, even while the game
is running. When two mods change the same shader, the one whose file name sorts last wins.

To share a mod, press **Save** first: a Live Update build only works on the renderer you're using. Share the `.kshader`
file, and the `.kgraph` file beside it too if you want others to be able to edit it.

### HLSL

Tick **Raw HLSL** to write the body of `float3 Shade(ShadeInputs s)` yourself, or add a **Custom HLSL** node for one step
of a graph. Both run in the preview as well as in the game.

| Field | Holds |
| --- | --- |
| `colour`, `alpha` | The game's finished pixel and its opacity |
| `albedo`, `light` | The colour before lighting, with all of a material's layers, and the light (`colour / albedo`) |
| `normal`, `view`, `up` | The surface direction, towards the camera and towards the sky, in camera space; `has_normal` says if the normal is real |
| `light_dir`, `light_colour` | The sun or main light |
| `uv`, `screen` | The first texture coordinate, and the pixel's position in pixels |
| `time` | Seconds since the game started, wrapping hourly |

Helpers: `ModKeyLight(s, towards, up, side)`, `ModStep(edge, x)`, `ModBands(x, count)`, `ModFresnel(s)`, `ModOutline(s, width)`,
`ModHalftone(s.screen, amount, cell)`, `ModLuminance(colour)` and `ModNoise(uv)`. If `Shade` returns a value that isn't a
number, such as after dividing by zero, the game's own colour is drawn for that pixel.

### From the command line

```sh
KameoShaderEditor --build "Cel shade" kameo,rubble shader_mods/my_mod.kshader
```

The first argument is a preset's name or a `.kgraph` file. Targets are `characters`, `everything`, or names from
`targets.json` in `res/shader_mods/source.zip`. The result goes to the `.kshader` file with `.log` added to its name.

## Command line

The add-on's Python package also runs on its own (Python 3.10+ with NumPy), from the folder holding
`kameo_modkit`:

```sh
python -m kameo_modkit gltf   assets/007183BF_01.mdl  work/kameo.gltf      # a model as glTF, to edit anywhere
python -m kameo_modkit import assets/007183BF_01.mdl  work/edited.gltf  work/007183BF_01.mdl
python -m kameo_modkit kmod   work/my_mod.kmod "assets/007183BF_01.mdl=work/007183BF_01.mdl" --name "My Mod" --author me
python -m kameo_modkit info   assets/007183BF_01.mdl                     # an archive's sections and asset types
```

Model files are named by a hash of the character's name and a skin number: `007183BF_00`, `_01` and `_02` are
Kameo's Classic, Gothic and Coyote costumes, and the others are warriors' alternate skins. Kameo's everyday outfit and
the warriors' standard looks live in a level archive, `011C2CC3.lvl`, and can't be edited yet.

## How it works

For contributors; the source is in [`tools/modkit`](../tools/modkit).

- Models are CAFF archives. The modkit reads their rendergraphs (skeleton, meshes, materials, texture palette) and
  writes edited archives back: vertex data, index buffers, new draws, new textures.
- The game draws a model from command streams in its `.stream` section, which bake in each draw's primitive type,
  index count, and the vertex shader's position and UV scales. The importer keeps them in step: it patches the counts,
  never changes the scales, and inserts new draws as packets beside the draw they copy.
- A `.kmod` is a list of byte-range differences per file, checked against the original's hash, which the launcher
  applies into `mods_build` (`src/launcher/kmod.cpp`).
- The game has thousands of pixel shaders, one per combination of effects, so a shader mod can't name the ones it
  replaces. `tools/shaders/build_shader_mod_source.py` translates them all and adds calls into
  `src/gpu/shaders/hlsl/shader_mod.hlsli`, giving `res/shader_mods/source.zip`. `Shade` runs at the very end of a
  shader, because shaders add to their colour after fog and would paint over anything returned sooner; fog is put back
  over its result. Run the script again if the game's shaders change. The editor compiles `Shade` into each target.
- A shader's base colour can't be read from its code, since materials layer and tint textures in too many ways. So
  each shader also holds a copy of itself reading white ambient, black lights, no shadow and no fog in place of those
  constants, whose output is the surface's colour. The compiler drops the copy when a mod doesn't read it.
- A `.kshader` maps game shader hashes to compiled stand-ins. `src/gpu/shaders/shader_mods.cpp` swaps them in by hash.
  It can also list mesh keys, hashes of the start of vertex buffers, which limit it to those meshes, and make it a
  costume (`src/engine/dlc/dlc_skin_names.cpp`): the Bonus costume line's next-costume call gains a step between her
  last costume and her everyday outfit for each such package, which load no model. The game only steps forward, so
  `src/ui/bonus_costumes.cpp` steps back by running the same call with its find-next told where to land. A save keeps the choice as `s` and
  six hex digits of a hash of the package's name, a suffix no model file has. Only the chosen costume's stand-ins are
  swapped in, and they draw only while no costume model is loaded, as her costumes reuse parts of her everyday outfit,
  eyes among them.
  Pipelines are cached by shader module, so a swap builds new pipelines by itself, and looking at the folder twice a
  second is all live reload needs.
