# anim

The offline half of `main/anim/`: bakes a glTF 2.0 animation into a pack entry
or C tracks, and prints what baked tracks hold. What a track is, how to author one and how to
target a new property is in [docs/Animation-Tracks.md](../../../docs/Animation-Tracks.md).
Nothing here runs on the board.

| File | What it does |
|---|---|
| [tracks_asset.py](tracks_asset.py) | The one writer and reader of the `TRCK` pack entry: bakes the animation a `NAME.anim.toml` names, with the checks every bake makes. `r3d/build_pack.py` calls it. |
| [bake_tracks.py](bake_tracks.py) | Reads one named animation with `tools/gltf/gltf_read.py` and writes `NAME_tracks_generated.{c,h}` from the tracks `tracks_asset.py` bakes: a track per channel, node TRS or `KHR_animation_pointer`, keys and interpolation as authored. |
| [sample_tracks.sh](sample_tracks.sh), [sample_tracks_main.c](sample_tracks_main.c) | Builds a program over one baked animation and prints every track every N ms, or a camera node as a poses file. |

```sh
python tools/anim/bake_tracks.py ASSET.glb --animation NAME --name PREFIX --out-dir DIR
tools/anim/sample_tracks.sh --tracks DIR/PREFIX_tracks_generated.c:PREFIX --every 250
```

The reader and writer are in [`tools/gltf/`](../gltf/); the sampler in `gltf/gltf_read.py` is the reference: `tests/test_anim_bake.py`
and `tests/test_anim_tracks_asset.py` hold the C runtime to it; `tests/anim_probe.py` writes the clip the host suite samples.
