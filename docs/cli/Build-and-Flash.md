# Build and flash

Makes or writes an image, or says which image runs. `autana help build` lists
the same commands.

<!-- generated: cli-build check: python scripts/autana/cli_reference.py --check -->
| Command | What it does |
|---|---|
| `autana build [rel\|dev\|diag] [--perf-scope [--frame-laps]] [--layout-seed N] [--hot-tunables] [--configure]` | build without a board lock; dev when omitted; --layout-seed N > 0 pads the layout, 0 is the plain build; --frame-laps adds the laps inside the frame stages; --hot-tunables adds live hot-path knobs; --configure only writes sdkconfig and compile_commands.json |
| `autana build diag --check` | the diagnostics build plus the complexity ratchet, no board |
| `autana flash [rel\|dev\|diag] [--quiet] [--perf-scope [--frame-laps]] [--layout-seed N] [--hot-tunables]` | build, then flash under the lock; dev when omitted; --quiet logs only; --perf-scope (diag) builds the perf-scoped image, --frame-laps adds the laps inside its frame stages |
| `autana buildid [--json]` | the BUILD_ID the board runs, to check against what was flashed |
<!-- /generated: cli-build -->

Every compile holds the host-wide heavy lock; flash writes and captures do not.
The wait uses `--wait` or the board lock's default, and a timeout exits 75
([Device-Lock.md](Device-Lock.md#heavy-host-work)).

`BUILD_ID` is the image's ELF hash and proves the write, not the boot
([what a flash proves](Flash-and-Captures.md#what-a-flash-proves)). Which
ESP-IDF is used and where builds live: [Build-Environment.md](../tools/Build-Environment.md).
`--layout-seed` pads the layout: [Layout-Noise.md](../tools/Layout-Noise.md).
