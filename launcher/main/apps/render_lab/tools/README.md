# Render Lab tools

Host-only scripts; the firmware build skips this folder. The render harness
itself is [`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

## Host renders

```sh
./launcher/main/apps/render_lab/tools/render_lab_render_host.sh
```

Writes every declared scene under `tools/results/render/render_lab/`:
`gouraud-landscape.bmp` is the shaded cube, `cornell-landscape.bmp` the
software ray-traced room. The integer scenes are pinned with the HUD hidden;
the HUD renders are `|nopin`, since its fps readout is a `double` printed
with `"%.1f"`, and so are the Cornell scenes, which are float throughout.
The fps text comes from the host fixture - time the board with a device
capture. The Gouraud scene rotates when stepped over several frames.

## Images in the docs

`doc_images.sh` here makes these in `docs/images/overview/`, run by
`launcher/tools/render/render_doc_images.sh`; see "Images in these docs" in
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

| Image | Shows |
|---|---|
| `render-lab-cube.png`, `render-lab-cube.gif` | the Gouraud cube; the GIF plays the rotation forward and back |
| `render-lab-cornell.png` | the ray-traced Cornell box, fully resolved, no HUD |
| `render-lab-sponza.gif` | the start of the Sponza flythrough |

## Sponza poses

`sponza_poses.txt` is the flythrough as a poses file for
[`tools/r3d/report_triangle_sizes.sh`](../../../../tools/r3d/README.md#triangle-sizes):
a pose every 5 s, the poses `suite_sponza_perf.c` times.
`gen_sponza_poses.sh` regenerates it after the flythrough changes.

```sh
./launcher/tools/r3d/report_triangle_sizes.sh \
    --mesh launcher/main/apps/render_lab/sponza_mesh_generated.c:sponza_mesh \
    launcher/main/apps/render_lab/tools/sponza_poses.txt
```
