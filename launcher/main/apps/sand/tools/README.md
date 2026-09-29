# Sand tools

Host-only scripts; the firmware build skips this folder. Each `report_*.sh`
names what it measures in its own header. The render harness the two
`*_render_host.sh` scenes use is
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

## Host renders

```sh
./launcher/main/apps/sand/tools/sand_menu_render_host.sh    # title and options screens
./launcher/main/apps/sand/tools/sand_sim_render_host.sh --video
```

`sand_sim_render_host.sh` steps the portable simulation through a volcano,
lake and grove with scripted tilt through the input filter, painting
through the shared material shading code into the host framebuffer. Its
landscape render is checked for size only (`|nopin`).
`make_volcano_clip.py` renders that scene, rotates each panel by its
scripted tilt angle, and encodes the loop as a GIF.

## Images in the docs

`launcher/tools/render/render_readme_images.sh` makes these in
`docs/images/overview/`; see "Images in these docs" in
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

| Image | Shows |
|---|---|
| `sand-menu.png` | the title screen |
| `sand-simulation.gif` | the volcano, lake and grove inside its turning panel, made by `make_volcano_clip.py`; `--contact <path>` also writes a six-frame inspection strip |
