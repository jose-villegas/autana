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
| Code pad | none | 0 to 4064 B in 32 B steps, one icache way |
| Rodata pad | none | 0 to 8128 B in 64 B steps, one dcache way |
| Which sources | none | every source of the main component, taken from its `SRCS` |
| Size | none | a hash of the seed and the source's path, so it does not shift when another source is added |

`launcher/tools/build/layout_pad.py` is the mapping and
`launcher/tools/build/layout_pad.h` the pad: a retained section of its own
(`.text.layout_pad`, `.rodata.layout_pad`) that nothing refers to, which is why
`--gc-sections` keeps it and the linker places it first among the object's
sections. Functions in `render/` start on a cache line and a pad is a whole
number of lines, so the in-line offsets `render/code_layout.h` pins hold for
every seed; `launcher/tools/render/code_layout.py --check` passes on a seeded
build.

A seeded image has its own `BUILD_ID`, and the same seed builds the same image.

## The pilot

`launcher/tools/perf/layout_pilot.py run` flashes one image per seed, captures
each suite R times on it, and `report` prints, for every row:

| | |
|---|---|
| sigma_run | the spread of runs inside one flash |
| sigma_layout | the spread between seeds' means beyond what sigma_run explains |
| R* | runs per flash that minimise the cost of a given precision: ceil(sqrt(c_flash / c_run * sigma_run² / sigma_layout²)) |
| K | seeds per side so the 95% interval on B/A is within ±0.1% or ±0.5% |
| Shapiro-Wilk p | whether the seed means look normal; a low p means layouts fall into modes |

The flash cost counts the build, the flash and the boot. A comparison's floor
is its own sigma_layout, measured from its own captures.
