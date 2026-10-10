# Layout noise

A change that adds or removes bytes ahead of hot code moves every address
behind it. The instruction cache (32 KB, 8 ways, 32 B lines) repeats every
4 KB and the data cache (64 KB, 8 ways, 64 B lines) every 8 KB, so the same
loop can start fighting different neighbours for a set, and a row can move a
tenth of a percent with no timed code touched<sup>[[43]](../Citations.md#43)</sup>. A comparison of two builds
cannot tell that from a real change unless it also measures how much a
layout alone moves the row.

## The seeded build

`--layout-seed N` pads the image so a seed picks one layout<sup>[[44]](../Citations.md#44)</sup>:

```sh
autana build diag --layout-seed 3
autana suite run_sponza_perf_suite --flash --perf-scope --layout-seed 3
```

| | Seed 0 | Seed N > 0 |
|---|---|---|
| Image | the plain build, section for section | a never-run pad ahead of each source's code and rodata |
| Code pad | none | 0 to one instruction-cache way less a line, in whole lines |
| Rodata pad | none | 0 to one data-cache way less a line, in whole lines |
| Which sources | none | every source of the main component, taken from its `SRCS` |
| Size | none | a hash of the seed and the source's path, so it does not shift when another source is added |

The line size, cache size and ways come from the build's sdkconfig, and
`main/CMakeLists.txt` hands them to `launcher/tools/build/layout_pad.py`, the
mapping. `launcher/tools/build/layout_pad.h` is the pad: a retained section of
its own (`.text.layout_pad`, `.rodata.layout_pad`) that nothing refers to,
which is why `--gc-sections` keeps it and the linker places it first among the
object's sections. Functions in `render/` start on a cache line and a pad is a
whole number of lines, so the in-line offsets `render/code_layout.h` pins hold
for every seed; `launcher/tools/render/code_layout.py --check` passes on a
seeded build. A release build refuses a seed.

A seeded image has its own `BUILD_ID`, and the same seed builds the same image.

## The pilot

`launcher/tools/perf/layout_pilot.py run` flashes one image per entry of
`--seeds`, captures each suite R times on it, and `report` prints, for every row:

| | |
|---|---|
| sigma_run | the spread of runs inside one flash |
| sigma_flash | the spread between flashes' means beyond what sigma_run explains: layout plus boot-to-boot |
| R*<sup>[[41]](../Citations.md#41)</sup> | runs per flash that minimise the cost of a given precision: ceil(sqrt(c_flash / c_run * sigma_run² / sigma_flash²)) |
| K | flashes per side so the 95% interval on B/A is within +-0.1% or +-0.5%, at the measured runs per flash |
| Shapiro-Wilk p<sup>[[42]](../Citations.md#42)</sup> | whether the flash means look normal; with fewer than about 15 flashes it cannot judge |

Repeating a seed (`--seeds 1 1 1`) flashes the same image again, so that run's
sigma_flash is boot-to-boot alone; the difference to a run of distinct seeds is
the layout. The flash cost counts the build, the flash and the boot. A
comparison estimates sigma_flash from its own captures. See
[Performance comparison](../../launcher/tools/perf/README.md) for seeded revision
comparisons and A/A calibration.
