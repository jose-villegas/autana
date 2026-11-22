# anim

The offline half of `main/anim/`: bakes a glTF 2.0 animation<sup>[[17]](../../../docs/Citations.md#khronos-gltf-2-0)</sup> into a pack entry
and prints what a clip's tracks hold through the device's
sampler. What a track is, how to author one and how to target a new property
is in [docs/render/animation/Animation-Tracks.md](../../../docs/render/animation/Animation-Tracks.md).
`track_host.py --poses` takes a `.anim.toml`: a poses file is in the source
frame, and a built pack's tracks are in the engine frame
([the offline tools](../../../docs/render/offline/Mesh-Import.md#the-offline-tools)).
Nothing here runs on the board.

A `NAME.anim.toml` can name a `NAME.keys.toml` as its source.
`keys_file.py` builds it into glTF in memory and derives Catmull-Rom<sup>[[20]](../../../docs/Citations.md#catmull-1974)</sup>
tangents when a cubic track gives none, and for camera shorthand translation.
See [Keys files](../../../docs/render/animation/Animation-Tracks.md#keys-files).
The field table comes from the C component declarations.

| File | What it does |
|---|---|
| [tracks_asset.py](tracks_asset.py) | The one writer and reader of the `TRCK` pack entry: reads the animation a `NAME.anim.toml` names with `tools/gltf/gltf_read.py` and bakes TRCK bindings for node transforms and property pointers, with string-table paths, component codes, field names, value types and a scene or skeleton root. The camera yfov pointer is converted to short-axis FOV units. `r3d/build_pack.py` calls it. |
| [keys_file.py](keys_file.py) | Builds either form of `.keys.toml` into glTF bytes for `gltf_read.load_asset()`. |
| [scene_fields.py](scene_fields.py) | Reads the component tags and bindable fields from C. |
| [track_host.py](track_host.py), [track_host.c](track_host.c) | Prints every track of a clip's `TRCK` entry every N ms, or a camera node as a poses file, through `main/anim/`'s reader and sampler. A `.anim.toml` is first baked into a scratch pack of just that clip. Its poses are right only from a `.anim.toml`. `r3d/poses.py` gets a scene camera's poses from it. |

```sh
python tools/anim/track_host.py PATH/NAME.anim.toml --every 250
python tools/anim/track_host.py PATH/NAME.anim.toml --every 5000 --poses camera 184 224 0.62 6
```

The reader and writer are in [`tools/gltf/`](../gltf/); the sampler in
`gltf/gltf_read.py` is the reference. `tests/test_anim_bake.py` holds the C
runtime to it through `track_host`, `tests/test_track_host.py` holds
`track_host`'s build and poses, and `tests/anim_probe.py` writes the clip that
`suite_anim_tracks.c` holds to it on the host. `tests/test_anim_tracks_asset.py`
holds `tracks_asset.py`'s writer and reader to each other.

`gltf/gltf_read.py` owns the camera yfov conversion, including the static
aspect ratio (1 when absent). `test_anim_bake.py` checks square, landscape
and portrait conversions and cubic tangents. Joint-only clips of one skin
use skeleton paths from each joint's root joint, excluding non-joint
parents; scene clips use node names. The C sampler accepts both
roots, while `anim_bind()` resolves scene clips.
