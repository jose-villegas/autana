# Boot Anim

| File | Purpose |
|---|---|
| [report_boot_anim_perf.py](report_boot_anim_perf.py) | Formats a boot animation performance capture. |
| [report_boot_anim_perf.sh](report_boot_anim_perf.sh) | Captures and reports boot animation performance. |

## Where the animation lives

| File | Holds | Regenerate |
|---|---|---|
| `main/boot/boot_anim_timeline.json` | Timing and single settings: everything but the camera and space, edited by hand | `python tools/gen/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json main/boot/boot_anim_motion.glb > main/boot/boot_anim_timeline.h` |
| `main/boot/boot_anim_motion.glb` | The camera and the space: a glTF animation named `boot_motion` with nodes `camera` and `space`, authored in any glTF tool | authored |
| `main/boot/boot_anim_tracks_generated.c` | That animation as C tracks | `python tools/anim/bake_tracks.py main/boot/boot_anim_motion.glb --animation boot_motion --name boot_anim --out-dir main/boot` |

`boot_anim.h` samples the tracks with the engine's
[animation tracks](../../../docs/Animation-Tracks.md) and converts each
frame's values to mat4i's fixed point in `render/r3d_trs.h`. The space's
rotation is a quaternion slerp between its keys. Frames render on a host with
`tools/render/scenes/boot_anim_render_host.sh` (`--video` for the whole
animation).
