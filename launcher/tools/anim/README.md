# anim

The offline half of `main/anim/`: bakes a glTF 2.0 animation into a pack entry
and prints what a clip's tracks hold through the device's
sampler. What a track is, how to author one and how to target a new property
is in [docs/Animation-Tracks.md](../../../docs/Animation-Tracks.md).
`track_host.py --poses` takes a `.anim.toml`: a poses file is in the source
frame, and a built pack's tracks are in the engine frame
([the offline tools](../../../docs/render/Mesh-Import.md#the-offline-tools)).
Nothing here runs on the board.

A camera path without Blender is a `NAME.keys.toml` that a `.anim.toml` names
as its source; `camera_keys.py` builds it into glTF in memory at the bake. The
TOML sets `node` and `animation`, with `[[keys]]`
tables holding `t` in seconds, `eye = [x,y,z]` and `look_at = [x,y,z]`.
Times start at zero and increase strictly. Translation uses CUBICSPLINE
Catmull-Rom tangents; rotation uses LINEAR quaternions, local -Z forward,
world +Y up, with successive quaternions in the same hemisphere. A repeated
first key at the end wraps the translation tangents for a smooth loop.
The tool needs only Python's standard library. Vertical views are refused
because +Y up cannot define their roll.

| File | What it does |
|---|---|
| [tracks_asset.py](tracks_asset.py) | The one writer and reader of the `TRCK` pack entry: reads the animation a `NAME.anim.toml` names with `tools/gltf/gltf_read.py` and bakes TRCK bindings for node TRS and camera yfov, with string-table paths, component codes, field names, value types and a scene or skeleton root. Camera values and cubic tangents use engine short-axis FOV units. `r3d/build_pack.py` calls it. |
| [camera_keys.py](camera_keys.py) | Builds a camera `.keys.toml` into glTF bytes; `gltf/gltf_read.py`'s `load_asset` calls it. |
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
