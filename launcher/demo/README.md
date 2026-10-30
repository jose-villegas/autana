# Demo assets

Reference content for documentation, CI and host renders. Renderer content
ships in firmware only when an app [selects it](../../docs/assets/Building-Packs.md#packs).

Firmware inputs (`.toml`, clip `.glb`) sit outside `source/`, which is Git LFS
and absent from firmware clones. No baked mesh is committed: each is a bake
product the build fetches by its row in `launcher/bakes.lock`
([assets](../../docs/assets/README.md)).

- [Sponza](sponza/sponza.scene.toml) is the reference scene for baked lighting,
  geometry, fidelity and frame cost. Its [import](sponza/sponza.import.toml)
  names the OBJ, MTL and textures in `source/`; attribution is in
  [copyright.txt](sponza/source/copyright.txt). The flythrough animation
  lives beside the scene. MTL and attribution stay
  text. Fetch sources with `git lfs pull --exclude=""` before importing or
  rendering a reference.
- [Capybara](capybara/capybara.scene.toml) stands on a meadow, a plain square, under
  one sun, lit directly and baked per vertex. Its
  [import](capybara/capybara.import.toml) names the Blender source, whose glTF
  export is a cached bake the import reads the bind pose from; the
  [skinned lighting measurements](../../docs/render/animation/Skinned-Lighting.md) read
  that export too.
- [Bistro](bistro/bistro.scene.toml) is an open street with a far plane, for
  measuring what distance buys: a plaza, a long lane and a camera loop that
  walks the street and flies over the roofs, on a ground plane that reaches
  the horizon. Its [source](bistro/source/copyright.txt) is a reduced copy of
  Amazon Lumberyard Bistro's exterior (CC BY 4.0), made by
  [`reduce_source.py`](../tools/r3d/source/reduce_source.py); the attribution,
  licence and the command that made it sit beside it.
