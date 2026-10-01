# Performance regions

Development and diagnostics images expose the S3 performance counters around
named stages. Select one region and one event with the console:

```
PERF <region|off> [event]
```

`PERF <region>` selects retired instructions by default. `PERF off` stops
the next region from starting. A successful selection replies with `PERF_OK`;
an unknown region or event replies with `PERF_ERR`.

Each matching pass prints its cycles and selected event count, for example:

```
PERF app.stage cycles=312000 insn=221400
```

Regions are registered by the code that brackets them; an app's region
names are listed in that app's docs.

Event names and their Xtensa select/mask pairs are in
[`../../launcher/main/util/perf_region.c`](../../launcher/main/util/perf_region.c).
Each region is one counter window on the frame task's core; regions do not
nest. The API and all registered regions are absent from release images.
