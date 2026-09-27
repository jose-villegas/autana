# Gen

| File | Purpose |
|---|---|
| [gen_boot_anim_image.py](gen_boot_anim_image.py) | Bakes the boot image into a C header. |
| [gen_boot_anim_timeline.py](gen_boot_anim_timeline.py) | Bakes the boot animation timeline. |
| [gen_gfx_dither_patterns.py](gen_gfx_dither_patterns.py) | Bakes the ordered-dither threshold ranks `gfx_dither.h` reads. |
| [gen_gfx_palette_standard.py](gen_gfx_palette_standard.py) | Bakes standard graphics palette data. |
| [gen_icons.py](gen_icons.py) | Bakes icon artwork and metadata into C headers. |
| [gen_ridge_curve.py](gen_ridge_curve.py) | Bakes a ridge line from the source image. |
| [gen_ui_layout.py](gen_ui_layout.py) | Bakes authored screen layouts into C headers. |
| [gen_zeta_curve.py](gen_zeta_curve.py) | Bakes the boot animation curve. |
| [gfx_palette_gen.c](gfx_palette_gen.c) | Portable palette table calculations shared by host tools and tests. |
| [gfx_palette_gen.h](gfx_palette_gen.h) | Declarations for the palette table calculations. |

## The rules every generator follows

Apps keep their own generators in `apps/<name>/tools/`; the rules are the
same. `grep -rl "GENERATED FILE" launcher/main` lists every output.

**The output is checked in, beside the code that reads it.** A build-time
generator would put Python on the critical path of every clean build.

**The output says so, and says how.** Its first line is a banner naming the
exact command that regenerates it, where someone about to hand-edit it will
see it first. Commit the raw output; never run the formatter over it.

**The generator validates itself before emitting anything.**
`gen_zeta_curve.py` checks its zeta against known values and exits rather
than print a plausible table of wrong numbers.

**The shipped file is tested independently of the generator** - the rule
with teeth, since the file in the repo can be stale, hand-edited, or made by
an older version of the constants. Test it against the underlying
mathematics where there is some (`suite_boot_anim.c` checks the curve meets
the axis at every known zero and nowhere else). An asset with no math behind
it, such as the boot photograph, is pinned by `_Static_assert`s on its shape
plus a visual check.

**A tool that regenerates for you picks one of two policies.** State a
browser is actively editing goes to a scratch copy and reaches the tree only
on an explicit save; a file on disk the generator merely mirrors is
regenerated in place whenever the source is newer. The boot animation
editor does both: the timeline is the first kind, the photograph the second.
