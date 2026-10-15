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
| `main/boot/boot.png` | The photograph the animation crossfades to, drawn in the 448 x 368 reading frame | authored |
| `main/boot/boot.image.toml` | Names that photograph as the boot picture, turned into the panel's frame and baked into its own asset pack | `python tools/r3d/build_pack.py -o <dir>` (the firmware build runs it) |

`boot_anim_motion.c` reads the clip from its pack when boot starts; without
it (no assets partition, a bad pack, a malformed clip) the camera and space
hold an authored rest pose and the animation still draws. Boot reads the
picture (`boot_anim_photo.c`) on the second frame, one of the cheapest, so
checking its 330 KB pack neither delays the first frame nor stalls the
crossfade; without it the scene
stays where the photograph would have covered it. `boot_anim.h` samples the tracks with the engine's [animation tracks](../../../docs/Animation-Tracks.md) and converts each
frame's values to a `transformf_t` in `anim/anim_transform.h`. The space's
rotation is a quaternion slerp between its keys. Frames render on a host with
`tools/render/scenes/boot_anim_render_host.sh`; see [Watching a change](#watching-a-change).

## Spring floor

`boot_anim_floor.c` owns the spring sheet in PSRAM during boot. Each zeta
zero plucks its centre before the first fixed tick whose pen height reaches
that zero. Tick times start at zero and replay without dropping elapsed
ticks, so host captures and device frames share the same state. Allocation
failure leaves a flat floor.

The `floor_*` settings in `main/boot/boot_anim_timeline.json` control the
clock, spring coefficients and radial pluck. Offsets are Q16 metres;
velocities are metres per tick. Free edges mirror the boundary node.
The velocity-first update is stable for
`8 * floor_tension + floor_stiffness < 4 * 256`, with damping in `0..256`.
The generator enforces that bound.

Each zero the pen passes between `floor_pluck_from_ms` and
`floor_pluck_until_ms` plucks the floor; one passed outside that window does
not.

## Watching a change

Edit `main/boot/boot_anim_timeline.json`, then from `launcher/`:

```sh
python tools/gen/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json main/boot/boot_anim_motion.glb > main/boot/boot_anim_timeline.h
./tools/render/scenes/boot_anim_render_host.sh --video
```

`whole.avi` in the render folder is the whole animation, upright as the
board is held. The other renders are pinned, so a change to what boot draws
fails the run until it is re-pinned with `--update-baseline`; `whole.avi`
renders first and is written either way.
