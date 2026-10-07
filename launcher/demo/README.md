# Demo assets

Reference content for renderer documentation, CI and host renders. Nothing
here ships in firmware unless an app names its folder in `demo_assets.toml`:

```toml
demo = ["sponza"]
```

`launcher/tools/r3d/build_pack.py` follows these manifests below the folders
it searches. Deleting an app also removes its demo selection.
Firmware inputs (`.mesh`, `.toml` and clip `.glb`) sit outside `source/`, which uses Git LFS and is absent from firmware clones.

- [Sponza](sponza/sponza.scene.toml) is the reference scene for baked lighting,
  geometry, fidelity and frame cost. Its [import](sponza/sponza.import.toml)
  names the OBJ, MTL and textures in `source/`; attribution is in
  [copyright.txt](sponza/source/copyright.txt). The baked `.mesh` variants and
  the flythrough animation live beside the scene. Source assets use Git LFS;
  MTL and attribution stay text. Fetch sources with `git lfs pull --exclude=""`
  before importing or rendering a reference.
- [Capybara](capybara/) contains the Blender source and glTF export used by
  the [skinned lighting measurements](../../docs/render/Skinned-Lighting.md).
  Nothing in the firmware build reads these files.
