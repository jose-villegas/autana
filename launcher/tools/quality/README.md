# Quality

| File | Purpose |
|---|---|
| [complexity_gate.py](complexity_gate.py) | Checks cognitive complexity against the committed baseline. |
| [complexity_baseline.txt](complexity_baseline.txt) | Pinned cognitive complexity scores. |
| [misra_tidy_gate.py](misra_tidy_gate.py) | Counts selected clang-tidy findings per check and first-party source file. |
| [misra_tidy_baseline.txt](misra_tidy_baseline.txt) | Pinned clang-tidy finding counts. |
| [misra_check.sh](misra_check.sh) | Runs Cppcheck and MISRA checks on a firmware build. |
| [report_test_results.py](report_test_results.py) | Formats a device suite capture as a report. |
| [report_test_results.sh](report_test_results.sh) | Captures and reports all device suites. |

The diagnostics CI build runs both clang-tidy gates against its compile
database. `misra_tidy_gate.py` fails on an increased count or a new finding;
decreases print a reminder to run `--update-baseline`. Run
`python launcher/tools/quality/misra_tidy_gate.py --fix CHECK` to apply one
check's available fix-its to `launcher/main/`, then review the diff and rerun
the gate. Host Tests CI runs the portable suites with UBSan.

`scripts/gates/check_style_audit.py` keeps the steady-state path of
`launcher/main/` free of heap and console calls. That path is a function
named in a `.frame`, `.update`, `.frame_band` or `.draw` initializer, the
body of a `while (1)` or `for (;;)` loop with no `break`, `return` or `goto`,
and every function those call in the same file; `tests/` and `tools/`
folders are exempt.

| Rule | Flags on that path | Allowed |
|---|---|---|
| FRAME-PATH-HEAP | `malloc`, `calloc`, `realloc`, `free`, `heap_caps_` alloc and free | `if (p == NULL) p = malloc(...)`, which allocates once |
| FRAME-PATH-CONSOLE | `printf`, `fprintf`, `vprintf`, `vfprintf`, `puts`, `fputs`, `putchar` | `snprintf`; any call in a `CONFIG_LAUNCHER_DEVELOPMENT` or `CONFIG_LAUNCHER_SELFTEST` branch |

The C scan includes macro parentheses and switch default checks even when
their pinned count is zero. The unused return value check names C functions
in `launcher/.clang-tidy`. Cppcheck's MISRA addon checks conditions that use
integer expressions as booleans and the internal linkage rules.
