# Gen

| File | Purpose |
|---|---|
| [examples_doc.py](examples_doc.py) | Embeds tracked C examples in their owning documents. |
| [gen_boot_anim_timeline.py](gen_boot_anim_timeline.py) | Bakes the boot animation timeline. |
| [gen_draw_dither_patterns.py](gen_draw_dither_patterns.py) | Bakes the ordered-dither threshold ranks `draw_dither.h` reads. |
| [gen_draw_palette_standard.py](gen_draw_palette_standard.py) | Bakes standard graphics palette data. |
| [gen_skin_light_directions.c](gen_skin_light_directions.c) | Bakes unit directions with the math package's octahedral decoder. |
| [gen_skin_light_directions.sh](gen_skin_light_directions.sh) | Compiles and runs the host direction generator. |
| [package_headers.py](package_headers.py) | Writes each package README's header table from header banners. |
| [gen_zeta_curve.py](gen_zeta_curve.py) | Bakes the boot animation curve. |
| [draw_palette_gen.c](draw_palette_gen.c) | Portable palette table calculations shared by host tools and tests. |
| [draw_palette_gen.h](draw_palette_gen.h) | OKLab colour space and palette table calculations for host generators. |

## The rules every generator follows

Apps keep their own generators in `apps/<name>/tools/`; the rules are the
same. Every checked-in output, from the banner each one carries:

<!-- generated: generated-files check: python scripts/gates/check_generated_files.py --check-table -->
| Output | Generator | Run in | Command |
|---|---|---|---|
| [internal-pool.svg](../../../docs/memory/images/internal-pool.svg) | [memory_map.py](../build/memory_map.py) | `./` | `python launcher/tools/build/memory_map.py --chart pool docs/memory/images/internal-pool.svg` |
| [iram-components.svg](../../../docs/memory/images/iram-components.svg) | [memory_map.py](../build/memory_map.py) | `./` | `python launcher/tools/build/memory_map.py --chart components docs/memory/images/iram-components.svg` |
| [iram-what-if.svg](../../../docs/memory/images/iram-what-if.svg) | [iram_what_if.py](../build/iram_what_if.py) | `./` | `python launcher/tools/build/iram_what_if.py --chart docs/memory/images/iram-what-if.svg` |
| [boot_anim_curve.h](../../main/boot/boot_anim_curve.h) | [gen_zeta_curve.py](gen_zeta_curve.py) | `launcher/` | `python tools/gen/gen_zeta_curve.py > main/boot/boot_anim_curve.h` |
| [boot_anim_timeline.h](../../main/boot/boot_anim_timeline.h) | [gen_boot_anim_timeline.py](gen_boot_anim_timeline.py) | `launcher/` | `python tools/gen/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json main/boot/boot_anim_motion.glb > main/boot/boot_anim_timeline.h` |
| [draw_dither_patterns_generated.h](../../main/draw/draw_dither_patterns_generated.h) | [gen_draw_dither_patterns.py](gen_draw_dither_patterns.py) | `launcher/` | `python tools/gen/gen_draw_dither_patterns.py main/draw/draw_dither_patterns_generated.h` |
| [draw_palette_standard_generated.h](../../main/draw/draw_palette_standard_generated.h) | [gen_draw_palette_standard.py](gen_draw_palette_standard.py) | `launcher/` | `python tools/gen/gen_draw_palette_standard.py > main/draw/draw_palette_standard_generated.h` |
| [r3d_skin_light_directions.h](../../main/render/skin/r3d_skin_light_directions.h) | [gen_skin_light_directions.sh](gen_skin_light_directions.sh) | `launcher/` | `sh tools/gen/gen_skin_light_directions.sh > main/render/skin/r3d_skin_light_directions.h` |
| [crc32_tables.inc](../../packages/hash/src/crc32_tables.inc) | [gen_crc32_tables.py](gen_crc32_tables.py) | `./` | `python launcher/tools/gen/gen_crc32_tables.py launcher/packages/hash/src/crc32_tables.inc` |
<!-- /generated: generated-files -->

**The output is checked in, beside the code that reads it.** A build-time
generator would put Python on the critical path of every clean build.

**The output says so, and says how.** It opens with a banner naming the
exact command that regenerates it, where someone about to hand-edit it will
see it first. Commit the raw output; never run the formatter over it.
`scripts/gates/check_generated_files.py` reruns that command in CI and fails
on any difference (its `--check-table` judges the table above), so the command must name the
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
