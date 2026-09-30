# Boot Anim

| File | Purpose |
|---|---|
| [boot_anim_editor.html](boot_anim_editor.html) | Browser editor for boot animation timing and motion. |
| [boot_anim_editor_server.py](boot_anim_editor_server.py) | Local server for editor preview, baking, and device actions. |
| [boot_motion.py](boot_motion.py) | Converts the editor's keyframes to and from the glTF animation the firmware plays. |
| [report_boot_anim_perf.py](report_boot_anim_perf.py) | Formats a boot animation performance capture. |
| [report_boot_anim_perf.sh](report_boot_anim_perf.sh) | Captures and reports boot animation performance. |

## Where the animation lives

| File | Holds | Regenerate |
|---|---|---|
| `main/boot/boot_anim_timeline.json` | Timing and single settings: everything but the camera and space | `python tools/gen/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json main/boot/boot_anim_motion.glb > main/boot/boot_anim_timeline.h` |
| `main/boot/boot_anim_motion.glb` | The camera and the space, a glTF animation named `boot_motion` with nodes `camera` and `space` | authored: in the editor, or any glTF tool |
| `main/boot/boot_anim_tracks_generated.c` | That animation as C tracks | `python tools/anim/bake_tracks.py main/boot/boot_anim_motion.glb --animation boot_motion --name boot_anim --out-dir main/boot` |

`boot_anim.h` samples the tracks with the engine's
[animation tracks](../../../docs/Animation-Tracks.md) and converts each
frame's values to small3dlib's fixed point at `render/r3d_trs.h`.

The editor's Build & Flash writes all three and flashes. Export glTF
downloads the keyframes as a `.glb`, and Import glTF replaces the keyframes
with a file's: an ease is a cubic and comes back as the ease, a shape the
editor has no name for reads as linear, and a rotation, which a glTF stores
as a quaternion, comes back as one of its two Euler triples, the one nearest
the keyframe before. A rotation that turns about several axes at once is
stored as a key every 20 ms along its Euler path, so it imports as many
keyframes.
