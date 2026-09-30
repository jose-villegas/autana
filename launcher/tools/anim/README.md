# anim

The offline half of `main/anim/`: bakes a glTF 2.0 animation into C tracks and
prints what baked tracks hold. What a track is, how to author one and how to
target a new property is in [docs/Animation-Tracks.md](../../../docs/Animation-Tracks.md).
Nothing here runs on the board.

| File | What it does |
|---|---|
| [bake_tracks.py](bake_tracks.py) | Reads one named animation with `r3d/gltf_skin.py` and writes `NAME_tracks_generated.{c,h}`: a track per channel, node TRS or `KHR_animation_pointer`, keys and interpolation as authored. |
| [gltf_write.py](gltf_write.py) | Writes a binary glTF from nodes, cameras and animation channels, for a converter or a test that needs a file of its own. Standard library only. |
| [sample_tracks.sh](sample_tracks.sh), [sample_tracks_main.c](sample_tracks_main.c) | Builds a program over one baked animation and prints every track every N ms, or a camera node as a poses file. |

```sh
python tools/anim/bake_tracks.py ASSET.glb --animation NAME --name PREFIX --out-dir DIR
tools/anim/sample_tracks.sh --tracks DIR/PREFIX_tracks_generated.c:PREFIX --every 250
```

The sampler in `r3d/gltf_skin.py` is the reference: `tests/test_anim_bake.py`
holds the C runtime to it.
