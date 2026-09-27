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

Run from the repository root.

| Image | Command | Output to copy |
|---|---|---|
| `docs/images/overview/render-lab-cube.png` | `./launcher/main/apps/render_lab/tools/render_lab_render_host.sh -o <dir>` | `gouraud-landscape.png` |
| `docs/images/overview/render-lab-cornell.png` | the same | `cornell-landscape.png` |
| `docs/images/overview/render-lab-cube.gif` | below | |

The GIF is 100 frames of the cube, reversed back onto itself as a loop:

```sh
R=launcher/main/apps/render_lab/tools/results/render/render_lab
./launcher/main/apps/render_lab/tools/render_lab_render_host.sh
$R/render_lab_render --quarter 1 --no-hud --scene gouraud --frames 100 --dt 33 \
    -o $R/cube-motion.bmp --video $R/cube-motion.avi
ffmpeg -y -i $R/cube-motion.avi \
    -vf "fps=12,scale=336:-1:flags=lanczos,split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1:a=0,palettegen" \
    docs/images/overview/render-lab-cube-palette.png
ffmpeg -y -i $R/cube-motion.avi -i docs/images/overview/render-lab-cube-palette.png \
    -filter_complex "[0:v]fps=12,scale=336:-1:flags=lanczos,split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1:a=0[v];[v][1:v]paletteuse=dither=bayer" \
    -loop 0 docs/images/overview/render-lab-cube.gif
```
