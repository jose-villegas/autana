# Building packs

Part of the [asset docs](README.md). Which sources make a pack, the tools that write it, and how it reaches the board.

## Packs

A root is a source file nothing else names. `build_pack.py` finds the roots by
searching, so no list is kept:

| Root | Pack | Holds |
|---|---|---|
| `NAME.scene.toml` | `NAME` | its [scene entry](formats/SCNE.md) `NAME`, every mesh, skin, skeleton and clip it names |
| `NAME.import.toml` that no scene places | `NAME` | its variants' meshes |
| `NAME.anim.toml` that no scene names | `NAME` | its one clip, baked from its source |
| `NAME.sound.toml` | `NAME` | its one [sound](../audio/Sound-Assets.md), fetched as a locked `sound` bake of its WAV, FLAC, OGG or MP3 source |
| `NAME.image.toml` | `NAME` | its one picture, baked from the PNG it names |
| `NAME.icons.toml` | `NAME` | an image entry per [icon](../tools/Icon-Baker.md), named by the icon, baked from the set's atlas and SVGs |

A `NAME.pack.toml` makes its folder one pack, `NAME`: every root under that
folder puts its entries there instead of making a pack of its own, each with
its stem for id. Folder packs do not nest. `development = true` limits a
folder pack to development firmware images (`--image --development`); release
images omit it. Host builds and bake checks include every pack.
The engine's own content, which any app may use, is the folder pack
`engine` (`launcher/main/engine/`); an app's artwork is a folder pack the app
names, mounted while it runs.

The default search is `launcher/main/`. An app's `demo_assets.toml` adds
each folder it names in `demo = ["name", ...]` from `launcher/demo/`.
Only apps name demo assets; a `demo_assets.toml` inside `launcher/demo/` is
refused. Reference content ships only through these selections; see the
[demo assets](../../launcher/demo/README.md).

Ids are unique within a pack whatever their type: the reader finds an entry
by name, then checks its type, so a scene and its clip cannot share a stem,
and `build_pack.py` refuses them, naming both files. A mesh or clip two roots
name would be a shared asset, a pack of its own the others depend on; that
loader is not built, so `build_pack.py` refuses such a tree and names the
entry.

## Tools

| Tool | Does |
|---|---|
| `mesh_import.py --out DIR` | bakes a mesh and writes `<name>.mesh`, one entry, into DIR (a scratch bake) |
| `build_pack.py -o DIR [--image FILE]` | writes `DIR/<pack>.apak` for each root in `launcher/main` and its selected demo folders, and with `--image` the partition image |
| `build_pack.py --pack-of ID` | prints the pack that holds mesh `ID` |
| `build_pack.py [--bake-cache DIR] [--offline]` | takes every mesh from the bake cache by `launcher/bakes.lock` |
| `build_pack.py --skip-unlocked FILE` | leaves out each pack whose bakes only lack their lock rows and names it in FILE; any other missing bake still fails |
| `bake/bake.py list\|check\|fetch` | the bakes the packs need, each keyed on what makes it, and the lock of their bytes |
| `bake/bake.py bake` then `lock --from-run N` | makes the bakes the lock lacks into the cache; locks the ones a CI run made |
| `rebake.py` | rewrites one `.mesh`'s clusters and octree; a fixed point |

`launcher/tools/asset/asset_pack.py` writes the pack,
`launcher/tools/asset/asset_directory.py` the directory,
`launcher/tools/r3d/mesh_asset.py` and `lit_mesh.py` the mesh
entry, `launcher/tools/anim/tracks_asset.py` of the clip entry,
`launcher/tools/gfx/image_asset.py` of the image entry, and
`launcher/tools/audio/sound_asset.py` of the [sound entry](../audio/Sound-Assets.md); `asset_pack.c`,
`asset_directory.c`, `r3d_lit_mesh.c`, `anim_tracks.c`, `draw_image.c` and
`sound.c` are the respective readers.

Meshes and sounds are bake products, fetched by their lock rows; a clip
or image entry is baked from its source when the packs are built. Packs are never
committed: the firmware build, the host tests and
the render scripts each write the tree they are in, so there is no second copy
to keep in step.

## Flashing

The firmware build writes the packs to `assets/` in its build directory and
the partition image to `assets.bin`, listed with the partition's offset in
`flash_args`, so every `autana flash` writes the packs with the app.
`idf.py assets-flash` writes just that region, in seconds, after a change to
the meshes alone. The QEMU image carries it because it merges every file in
`flash_args`.
