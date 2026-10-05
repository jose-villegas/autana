# Performance comparison

`perf_compare.sh` runs the current report command repeatedly against two
revision projects. `summary.md` shows the worst and median value for every
named timing row. The worst values decide the verdict, preserving the same
conservative budget comparison; the medians show whether that headline came
from a typical run. The tool keeps the individual reports and verifies each
capture's build id against the one its flash booted, read from the run's log
(`autana buildid` after the run only when the log has none).

```sh
launcher/tools/perf/perf_compare.sh --runs 3 A B -- \
  bash launcher/main/apps/sand/tools/report_performance.sh --no-restore --perf-scope
```

The command runs from the current tree. Each revision checkout is appended
as `--project PATH`. A command that writes a raw capture names its
destination `@CAPTURE@`, and the numbers are read from that file
(`run_N.capture.log`), never from the report `autana` writes beside it:

```sh
launcher/tools/perf/perf_compare.sh --runs 3 A B -- \
  autana --wait 3600 suite run_sponza_perf_suite --runs 1 --flash --out @CAPTURE@
```

That form reads each `both cores: mean` capture line as a name-and-number
row. A command without `@CAPTURE@` gets the report path appended last
(`OUT.md`, the sand report form). Both flash through `autana suite --flash`.
Use `--no-restore` only when another capture will restore the board; the
default flashes `origin/main`'s release image after the comparison.

Each capture has a 30-minute deadline; set `--timeout SECONDS` for a different
limit. A command that exits 1 after writing timing rows remains a measurement:
that status can mean a test exceeded its budget. Raw `@CAPTURE@` commands must
also have a companion report whose `Ended` field says `complete`. The sand
report path confirms completion, validates the capture, and writes its timing
report before returning. A timeout, lost connection, wrong build, missing
measurement, or otherwise incomplete capture is recorded as failed. Two
consecutive failed captures stop the comparison.

The summary also calls out a possible whole-run slowdown when one run is at
least 0.2% above its side's per-row medians, with a similar increase on at
least 80% of rows. Similar means each aligned row is within 0.15 percentage
points of that run's median shift. The run stays in the worst values and
verdict. A change present in every run moves that side's medians instead, so
it remains a regression rather than being labelled a slow boot.

The layout noise floor, and how to measure it with seeded padding builds, is in
[Layout-Noise.md](../../../docs/tools/Layout-Noise.md); `layout_pilot.py` runs the pilot.
