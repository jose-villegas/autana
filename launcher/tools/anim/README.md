# anim

The offline half of `main/anim/`: bakes a glTF 2.0 animation into a pack entry
or C tracks, and prints what a clip's tracks hold through the device's
sampler. What a track is, how to author one and how to target a new property
is in [docs/Animation-Tracks.md](../../../docs/Animation-Tracks.md).
Nothing here runs on the board.

| File | What it does |
|---|---|
| [tracks_asset.py](tracks_asset.py) | The one writer and reader of the `TRCK` pack entry: bakes the animation a `NAME.anim.toml` names, with the checks every bake makes. `r3d/build_pack.py` calls it. |
| [bake_tracks.py](bake_tracks.py) | Reads one named animation with `tools/gltf/gltf_read.py` and writes `NAME_tracks_generated.{c,h}` from the tracks `tracks_asset.py` bakes: a track per channel, node TRS or `KHR_animation_pointer`, keys and interpolation as authored. |
| [track_host.py](track_host.py), [track_host.c](track_host.c) | Prints every track of a clip's `TRCK` entry every N ms, or a camera node as a poses file, through `main/anim/`'s reader and sampler. A `.anim.toml` is first baked into a scratch pack of just that clip. `r3d/poses.py` gets a scene camera's poses from it. |

```sh
python tools/anim/bake_tracks.py ASSET.glb --animation NAME --name PREFIX --out-dir DIR
python tools/anim/track_host.py PATH/NAME.anim.toml --every 250
python tools/anim/track_host.py --pack PACK --clip ID --every 5000 --poses camera 184 224 0.62 6
```

The reader and writer are in [`tools/gltf/`](../gltf/); the sampler in
`gltf/gltf_read.py` is the reference. `tests/test_anim_bake.py` holds the C
runtime to it through `track_host`, `tests/test_track_host.py` holds
`track_host`'s build and poses, and `tests/anim_probe.py` writes the clip that
`suite_anim_tracks.c` holds to it on the host. `tests/test_anim_tracks_asset.py`
holds `tracks_asset.py`'s writer and reader to each other.
