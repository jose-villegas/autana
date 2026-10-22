# Demo assets

Reference content for documentation, CI and host renders. Renderer content
ships in firmware only when an app [selects it](../../docs/assets/README.md#packs).

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
  [skinned lighting measurements](../../docs/render/Skinned-Lighting.md) read
  that export too.
- [Bistro](bistro/bistro.scene.toml) is an open street with a far plane, for
  measuring what distance buys: a plaza, a long lane and a camera loop that
  walks the street and flies over the roofs, on a ground plane that reaches
  the horizon. Its [source](bistro/source/copyright.txt) is a reduced copy of
  Amazon Lumberyard Bistro's exterior (CC BY 4.0), made by
  [`reduce_source.py`](../tools/r3d/source/reduce_source.py); the attribution,
  licence and the command that made it sit beside it.

The engine's [carreno_waltz](../main/engine/sounds/carreno_waltz.sound.toml)
is Teresa Carreño (1853–1917), *Gottschalk Waltz*, Op. 1. The composition is
public domain.

The [recording](https://commons.wikimedia.org/wiki/File:Teresa_Carre%C3%B1o_-_Gottschalk_Waltz.ogg)
is a virtual-piano rendering by Wikimedia Commons user Pracchia-78,
released into the public domain worldwide (`{{PD-self}}`).

The source OGG has SHA-256
`24821ef4dc0303908d83bc76c4b07ab024ce0d6ab4156b8419f94605cccd7f51`.

The excerpt spans 226.75–248.00 s, cut sample-exact at 44.1 kHz and stored
as stereo PCM16 FLAC. The recipe contains the reproducible cut command.
