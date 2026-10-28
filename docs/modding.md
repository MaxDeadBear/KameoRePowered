# Mods

Kameo: RePowered loads mods packaged as `.kmod` files. A mod can change level archives (`.lvl`) and character
models (`.mdl`): a new skin for one of Kameo's costumes, a hat, a recoloured warrior. A `.kmod` holds only the
differences from the game's own files, so it carries none of the game's data and can be shared freely.

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
2. In the storybook's **Bonus** pages, choose the costume the mod replaces.

A skin may replace one costume or all three; the launcher lists the files each mod changes (`007183BF_00.mdl` to
`_02.mdl` are the three costumes).

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
Kameo's three Bonus costumes, and the others are warriors' skins. Kameo's everyday outfit lives in a level archive
and can't be edited yet.

## How it works

For contributors; the source is in [`tools/modkit`](../tools/modkit).

- Models are CAFF archives. The modkit reads their rendergraphs (skeleton, meshes, materials, texture palette) and
  writes edited archives back: vertex data, index buffers, new draws, new textures.
- The game draws a model from command streams in its `.stream` section, which bake in each draw's primitive type,
  index count, and the vertex shader's position and UV scales. The importer keeps them in step: it patches the counts,
  never changes the scales, and inserts new draws as packets beside the draw they copy.
- A `.kmod` is a list of byte-range differences per file, checked against the original's hash, which the launcher
  applies into `mods_build` (`src/launcher/kmod.cpp`).
