# Gen

| File | Purpose |
|---|---|
| [gen_boot_anim_image.py](gen_boot_anim_image.py) | Bakes the boot image into a C header. |
| [gen_boot_anim_timeline.py](gen_boot_anim_timeline.py) | Bakes the boot animation timeline. |
| [gen_gfx_dither_patterns.py](gen_gfx_dither_patterns.py) | Bakes the ordered-dither threshold ranks `gfx_dither.h` reads. |
| [gen_gfx_palette_standard.py](gen_gfx_palette_standard.py) | Bakes standard graphics palette data. |
| [gen_icons.py](gen_icons.py) | Bakes icon artwork and metadata into C headers. |
| [gen_ridge_curve.py](gen_ridge_curve.py) | Bakes a ridge line from the source image. |
| [bake_ui_layout.py](bake_ui_layout.py) | Runs the C++ authored-layout baker built in `editor/build`. |
| [shared_helpers.py](shared_helpers.py) | Writes the shared-owner catalogue from file banners and public names. |
| [gen_zeta_curve.py](gen_zeta_curve.py) | Bakes the boot animation curve. |
| [gfx_palette_gen.c](gfx_palette_gen.c) | Portable palette table calculations shared by host tools and tests. |
| [gfx_palette_gen.h](gfx_palette_gen.h) | OKLab colour space and palette table calculations for host generators. |

## The rules every generator follows

Apps keep their own generators in `apps/<name>/tools/`; the rules are the
same. Engine outputs are listed here from their banners; each app's tools
README lists its own generated outputs.

<!-- generated: generated-files sha256=83779ceed0381dd0c699e0a0b08de01106e57d832ce5854344c71883f4552e9d -->
| Output | Generator | Run in | Command |
|---|---|---|---|
| [boot_anim_curve.h](../../main/boot/boot_anim_curve.h) | [gen_zeta_curve.py](gen_zeta_curve.py) | `launcher/` | `python tools/gen/gen_zeta_curve.py > main/boot/boot_anim_curve.h` |
| [boot_anim_image.h](../../main/boot/boot_anim_image.h) | [gen_boot_anim_image.py](gen_boot_anim_image.py) | `launcher/` | `python tools/gen/gen_boot_anim_image.py ../design/boot/boot.png > main/boot/boot_anim_image.h` |
| [boot_anim_timeline.h](../../main/boot/boot_anim_timeline.h) | [gen_boot_anim_timeline.py](gen_boot_anim_timeline.py) | `launcher/` | `python tools/gen/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json main/boot/boot_anim_motion.glb > main/boot/boot_anim_timeline.h` |
| [gfx_dither_patterns_generated.h](../../main/gfx/gfx_dither_patterns_generated.h) | [gen_gfx_dither_patterns.py](gen_gfx_dither_patterns.py) | `launcher/` | `python tools/gen/gen_gfx_dither_patterns.py main/gfx/gfx_dither_patterns_generated.h` |
| [gfx_palette_standard_generated.h](../../main/gfx/gfx_palette_standard_generated.h) | [gen_gfx_palette_standard.py](gen_gfx_palette_standard.py) | `launcher/` | `python tools/gen/gen_gfx_palette_standard.py > main/gfx/gfx_palette_standard_generated.h` |
| [icons_system.h](../../main/gfx/icons_system.h) | [gen_icons.py](gen_icons.py) | `launcher/` | `python tools/gen/gen_icons.py ../design/icons/system.png ../design/icons/system.json > main/gfx/icons_system.h` |
| [control_center_layout_generated.h](../../main/ui/control_center_layout_generated.h) | [bake_ui_layout.py](bake_ui_layout.py) | `launcher/` | `python tools/gen/bake_ui_layout.py "main/ui/control_center_layout.json" "main/ui/control_center_layout_generated.h"` |
| [ridge_curve_generated.h](../../main/ui/ridge_curve_generated.h) | [gen_ridge_curve.py](gen_ridge_curve.py) | `launcher/` | `python tools/gen/gen_ridge_curve.py ../design/boot/ridge.png main/ui/ridge_curve_generated.h` |
<!-- /generated: generated-files -->

**The output is checked in, beside the code that reads it.** A build-time
generator would put Python on the critical path of every clean build.

**The output says so, and says how.** It opens with a banner naming the
exact command that regenerates it, where someone about to hand-edit it will
see it first. Commit the raw output; never run the formatter over it.
`scripts/gates/check_generated_files.py` reruns that command in CI and fails
on any difference, and on a stale table above, so the command must name the
file it writes and use only inputs in the repository - see
[Generated-Files.md](../../../docs/tools/Generated-Files.md).

**The generator validates itself before emitting anything.**
`gen_zeta_curve.py` checks its zeta against known values and exits rather
than print a plausible table of wrong numbers.

**The shipped file is tested independently of the generator**: the rule
with teeth, since the file in the repo can be stale, hand-edited, or made by
an older version of the constants. Test it against the underlying
mathematics where there is some (`suite_boot_anim.c` checks the curve meets
the axis at every known zero and nowhere else). An asset with no math behind
it, such as the boot photograph, is pinned by `_Static_assert`s on its shape
plus a visual check.

**A tool that regenerates for you picks one of two policies.** State a
browser is actively editing goes to a scratch copy and reaches the tree only
on an explicit save; a file on disk the generator merely mirrors is
regenerated in place whenever the source is newer.
