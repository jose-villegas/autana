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
| `main/boot/boot_anim_motion.anim.toml` | Names that animation as the boot clip, baked into its own asset pack | `python tools/r3d/build_pack.py -o <dir>` (the firmware build runs it) |

`boot_anim_motion.c` reads the clip from its pack when boot starts; without
it (no assets partition, a bad pack, a malformed clip) the camera and space
hold an authored rest pose and the animation still draws. `boot_anim.h`
samples the tracks with the engine's [animation tracks](../../../docs/Animation-Tracks.md) and converts each
frame's values to a `transformf_t` in `anim/anim_transform.h`. The space's
rotation is a quaternion slerp between its keys. Frames render on a host with
`tools/render/scenes/boot_anim_render_host.sh` (`--video` for the whole
animation).
