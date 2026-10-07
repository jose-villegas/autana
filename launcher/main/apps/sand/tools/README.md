# Sand tools

Host-only scripts; the firmware build skips this folder. Each `report_*.sh`
names what it measures in its own header. The render harness the
`*_render_host.sh` scenes use is
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

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

## In a browser

```sh
./launcher/main/apps/sand/tools/web/setup_emsdk.sh   # once, unless emcc is on PATH
./launcher/main/apps/sand/tools/web/build_web.sh     # into web/dist/
python3 -m http.server -d launcher/main/apps/sand/tools/web/dist
```

`web/` compiles the simulation, the row painter and its clocks to
WebAssembly, with `web_sand.c` doing the app's frame and `app.js` the page's
input. The grid is the panel's own shape at the device's cell sizes;
landscape is that panel turned a quarter turn. The Pages workflow publishes
it under the site's `sand/` on every push to `main`.

## Images in the docs

`doc_images.sh` here makes these in `docs/images/overview/`, run by
`launcher/tools/render/render_doc_images.sh`; see "Images in these docs" in
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

| Image | Shows |
|---|---|
| `sand-menu.png` | the title screen |
| `sand-simulation.gif` | the volcano, lake and grove inside its turning panel, made by `make_volcano_clip.py`; `--contact <path>` also writes a six-frame inspection strip |
