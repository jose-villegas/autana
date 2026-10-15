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

The C scan includes macro parentheses and switch default checks even when
their pinned count is zero. The unused return value check names C functions
in `launcher/.clang-tidy`. Cppcheck's MISRA addon checks conditions that use
integer expressions as booleans and the internal linkage rules.

## Instruction counts

`icount_gate.py` compares a QEMU `--icount` serial log with the generated
`icount_baseline.txt`. Only named serial simulation and raster span kernels
are pinned; frame totals and whole suites would constrain unrelated code.
See [the testing guide](../../../docs/Testing-Guide.md#qemu-the-device-image-with-no-board)
for the suite command.

Both a rise and a fall beyond 0.5% fail. Lower an improved entry in the same
PR; raise one only with a stated reason in that PR. After two identical
captures, run `python launcher/tools/quality/icount_gate.py --update-baseline
LOG` to generate the sorted baseline. Missing, unexpected, empty and short
measurements fail, including during updates. A compiler or IDF bump
re-baselines in one deliberate commit.
