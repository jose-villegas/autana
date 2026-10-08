# Sand tools


Host-only scripts; the firmware build skips this folder. Each `report_*.sh`
names what it measures in its own header. The render harness the
`*_render_host.sh` scenes use is
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

## Generated files

<!-- generated: generated-files-sand sha256=bed1e804bdc4a2730410835527645a3c08ffd6bb2030cdd3f9aab5523bee7a4a -->
| Output | Generator | Run in | Command |
|---|---|---|---|
| [icons_dither.h](../icons_dither.h) | [gen_icons.py](../../../../tools/gen/gen_icons.py) | `launcher/` | `python tools/gen/gen_icons.py main/apps/sand/icons/dither.png main/apps/sand/icons/dither.json > main/apps/sand/icons_dither.h` |
| [icons_sand.h](../icons_sand.h) | [gen_icons.py](../../../../tools/gen/gen_icons.py) | `launcher/` | `python tools/gen/gen_icons.py main/apps/sand/icons/sand.png main/apps/sand/icons/sand.json > main/apps/sand/icons_sand.h` |
| [sand_palette256.h](../sand_palette256.h) | [report_shading_palette.sh](report_shading_palette.sh) | `launcher/` | `main/apps/sand/tools/report_shading_palette.sh main/apps/sand/sand_palette256.h` |
<!-- /generated: generated-files-sand -->

## Host renders

```sh
./launcher/main/apps/sand/tools/sand_menu_render_host.sh    # title and options screens
./launcher/main/apps/sand/tools/brush_screen_render_host.sh # brush screen, both orientations
./launcher/main/apps/sand/tools/sand_sim_render_host.sh --video
```

`sand_sim_render_host.sh` steps the portable simulation through a volcano,
lake and grove with scripted tilt through the input filter, painting
through the shared material shading code into the host framebuffer. Its
landscape render is checked for size only (`|nopin`).
`make_volcano_clip.py` renders that scene, rotates each panel by its
scripted tilt angle, and encodes the loop as a GIF.

## Images in the docs

`doc_images.sh` here makes these in `docs/images/overview/`, run by
`launcher/tools/render/render_doc_images.sh`; see "Images in these docs" in
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

| Image | Shows |
|---|---|
| `sand-menu.png` | the title screen |
| `sand-simulation.gif` | the volcano, lake and grove inside its turning panel, made by `make_volcano_clip.py`; `--contact <path>` also writes a six-frame inspection strip |
