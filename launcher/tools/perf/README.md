# Performance comparison

`perf_compare.sh` runs the current report command repeatedly against two
revision projects and writes the worst observed value for every named timing
row to `summary.md`. It keeps the individual reports and verifies each
capture's board build id.

```sh
launcher/tools/perf/perf_compare.sh --runs 3 A B -- \
  bash launcher/main/apps/sand/tools/report_performance.sh --no-restore --perf-scope
```

The report command runs from the current tree. Each revision checkout is
appended as `--project PATH`, followed by `OUT.md`. A command that writes a
raw capture puts `--out` last, so its destination is appended there:

```sh
launcher/tools/perf/perf_compare.sh --runs 3 A B -- \
  autana --wait 3600 suite run_sponza_perf_suite --runs 1 --flash --out
```

The second form reads `both cores: mean` capture lines as name-and-number
rows. Use `--no-restore` only when another capture will restore the board;
the default flashes `origin/main`'s release image after the comparison.

Each capture has a 30-minute deadline; set `--timeout SECONDS` for a different
limit. A failed capture is recorded and the next one runs. Two consecutive
failed captures stop the comparison.
