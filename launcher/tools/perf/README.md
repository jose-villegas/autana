# Performance comparison

`perf_compare.sh` runs one report command repeatedly at two revisions and
writes the worst observed value for every named timing row to `summary.md`.
It keeps the individual reports and records board build ids around every run.

```sh
launcher/tools/perf/perf_compare.sh --runs 3 A B -- \
  bash launcher/main/apps/sand/tools/report_performance.sh --no-restore --perf-scope
```

The report command runs from each revision checkout. Its `OUT.md` positional
argument is appended automatically. A command that writes a raw capture puts
`--out` last, so its destination is appended there:

```sh
launcher/tools/perf/perf_compare.sh --runs 3 A B -- \
  autana --wait 3600 suite run_sponza_perf_suite --runs 1 --flash --out
```

The second form reads `both cores: mean` capture lines as name-and-number
rows. Use `--no-restore` only when another capture will restore the board;
the default flashes `origin/main`'s release image after the comparison.
