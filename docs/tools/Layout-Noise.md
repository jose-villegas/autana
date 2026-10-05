# Layout noise

A change that adds or removes bytes ahead of hot code moves every address
behind it. The instruction cache (32 KB, 8 ways, 32 B lines) repeats every
4 KB and the data cache (64 KB, 8 ways, 64 B lines) every 8 KB, so the same
loop can start fighting different neighbours for a set, and a row can move a
tenth of a percent with no timed code touched. A comparison of two builds
cannot tell that from a real change unless it also measures how much a
layout alone moves the row.

## The seeded build

`--layout-seed N` pads the image so a seed picks one layout:

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
| R* | runs per flash that minimise the cost of a given precision: ceil(sqrt(c_flash / c_run * sigma_runÂ² / sigma_flashÂ²)) |
| K | flashes per side so the 95% interval on B/A is within +-0.1% or +-0.5%, at the measured runs per flash |
| Shapiro-Wilk p | whether the flash means look normal; with fewer than about 15 flashes it cannot judge |

Repeating a seed (`--seeds 1 1 1`) flashes the same image again, so that run's
sigma_flash is boot-to-boot alone; the difference to a run of distinct seeds is
the layout. The flash cost counts the build, the flash and the boot. A
comparison estimates sigma_flash from its own captures.

## Revision comparison

[`perf_compare.sh`](../../launcher/tools/perf/README.md) uses separate random
layout seeds for A and B, with random order within each A/B flash pair.
`A A` calibrates two seed sets of the same source. Each flash captures repeated
runs; their mean is one independent seed observation. The tool shares the
pilot's acquisition and variance calculations and uses the seeded build's
pad geometry directly.

The first pass has at least two runs per seed and enough seeds for the exact
permutation test to reach its testing alpha. Measured flash/run wall costs,
sigma_run and sigma_flash determine recommended runs per flash and required
seeds for the requested threshold. Extra passes select only inconclusive
rows, map them to capture test names, and use the largest recommended run
count. Required seeds select a planned look up to `--max-seeds`; the summary
states the first pass, cap and threshold. The random plan is replayable with
`--rng-seed` and recorded alongside captures.

The three-way result uses seed means. No change requires equivalence within
`--threshold PCT` through a Holm-adjusted TOST. Improved or regressed requires
a Holm-adjusted Welch difference on log seed means and a permutation test
that agrees. The permutation test is exact when feasible and Monte Carlo
otherwise; it reports n/a when the sample count cannot reach alpha. Anything
else is inconclusive, including a row still undecided at the cap. Holm adjusts
the difference and equivalence families separately; alpha is budgeted across
planned looks to account for repeated decisions. No individual row supplies
a noise floor for another row.

The displayed B/A is a ratio of arithmetic seed means. Its Welch interval
is on the geometric ratio from log seed means, so they can differ for skewed
layouts. Medians are descriptive. Instruction deltas are shown where existing
capture lines identify an unambiguous pairing with the timing row.
