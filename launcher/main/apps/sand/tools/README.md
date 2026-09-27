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

Run from the repository root.

| Image | Command | Output to copy |
|---|---|---|
| `docs/images/overview/sand-menu.png` | `./launcher/main/apps/sand/tools/sand_menu_render_host.sh -o <dir>` | `title-landscape.png` |
| `docs/images/overview/sand-simulation.gif` | `python launcher/main/apps/sand/tools/make_volcano_clip.py` | written in place; `--contact <path>` also writes a six-frame inspection strip |
