# The cognitive-complexity ratchet

`launcher/tools/complexity_gate.py` measures cognitive complexity with
clang-tidy's `readability-function-cognitive-complexity` check
(`launcher/.clang-tidy` holds its configuration) and ratchets every
first-party function's score against a committed baseline instead of a
fixed threshold.

## The compilation database

clang-tidy needs real include paths and defines to parse a file. The gate
builds one `compile_commands.json` (under `launcher/build.tidy/`,
gitignored, regenerated on every run) from two sources, merged with no
file counted twice:

- **The diagnostics build's own database**,
  `launcher/build.diag/compile_commands.json` - real esp32s3 flags,
  restricted to `launcher/main/` and `launcher/test/`. This is what
  reaches hardware-facing files: `app_sand.c`, `app_cube.c`,
  `app_diagnostics.c`, `main.c`, `gfx/gfx.c`, `ui/ui.c`,
  `ui/ui_launcher.c`, `board/board_esp32s3.c`, `boot/*.c`,
  `input/buttons.c`, `input/imu.c`, `input/touch.c`,
  `util/device_state.c`, `util/screenshot.c`, and every on-device test
  suite. esp-clang does not recognise three GCC-only Xtensa flags in that
  database (stripped) and has no bundled libc for the target (given
  `--sysroot`/`--gcc-toolchain` pointing at the same `xtensa-esp-elf` GCC
  install ESP-IDF itself uses, found under
  `~/.espressif/tools/xtensa-esp-elf/`).
- **`launcher/test/run_tests.sh --print-sources`/`--print-flags`** - the
  same host-portable file list and flags that script proves compile,
  used for whatever the diagnostics database does not contain: the
  host-only test-runner files (`host_main.c`, `heap_arena.c`) and
  `gfx/gfx_palette_standard.c`. esp-clang's own default target has no
  usable libc either, so these get the same `--sysroot`/`--gcc-toolchain`
  treatment against the host compiler `tools/find_cc.sh` resolves -
  never a second, independently-guessed compiler.
- **`apps/*/tools/*.c`** (sweep and report scripts, excluded from the
  firmware and the host build alike by long-standing convention) get the
  host route's flags plus one additional include path for their sibling
  headers, since each already has its own working host compile line in a
  `report_*.sh` beside it.

**Coverage is a checked rule, not a description.** Every `.c` file under
`launcher/main/` is walked directly from the filesystem, independent of
either database, and compared against what was actually measured. A file
neither source reaches, and that is not below, fails the gate by name:

| File | Why it is excluded |
|---|---|
| `main/apps/sand/tools/crossflow_bench.c` | uses C11 `timespec_get()`/`TIME_UTC`; esp-clang does not expose them under this project's `-std=c11` with the host route's headers, unrelated to the Xtensa target - host gcc compiles it fine (`report_crossflow.sh`) |

Vendored code (`launcher/components/`, `managed_components/`, and the
vendored Unity under `test/framework/`) is out of scope entirely - a
ratchet on this project's own functions has nothing to say about code it
did not write - and is never a source of a coverage gap, since it sits
outside `launcher/main/`. A `static inline` helper defined only in a
shared header (for example `sand_priv.h`'s `dest_row()`/`mark_rows()`) is
still invisible to this gate: clang-tidy's default scope is the file
actually being compiled, not headers it pulls in.

The gate fails loudly rather than passing quietly past a measurement gap:
a scan that finds zero functions, one that finds fewer than half the
baseline's function count, an unexcluded gap under `launcher/main/`, or
any `clang-diagnostic-error` (a partial parse can hide functions) all
fail the run immediately instead of being reported as clean.

## A ratchet, not a threshold

`launcher/tools/complexity_baseline.txt` records every measured
function's current score, one line per function
(`score<TAB>file:line<TAB>name`, sorted worst first) - a plain text file
meant to be diffed in review like any other. It is the source for a
function's current score; nothing here restates a count or a number that
the baseline itself already holds.

The gate compares today's scan against that file by **(path, function
name)**, never by line number, so a function that only moved because
something above it grew is not treated as a complexity change:

- a function whose score **rose** above its baseline entry fails the gate
- a function whose score **fell** passes, with a note - lowering the
  baseline is only ever `--update-baseline`, run by a person on purpose,
  never automatic, so an improvement has to be noticed before it sticks
- a function with **no baseline entry** (new, or the new half of a
  rename) is judged against `NEW_FUNCTION_THRESHOLD` (15) instead
- a baseline entry with **no matching function** (the old half of a
  rename, or a real deletion) is reported as stale and otherwise
  ignored - it costs nothing to leave in the file until the next
  `--update-baseline` regenerates it

## Using it

Needs a diagnostics build first - `launcher/build.diag/compile_commands.json`
is one of the gate's two sources, and a missing one fails with a message
saying so rather than a stack trace:

```sh
./launcher/tools/build_diag_check.sh                            # once, several minutes
python launcher/tools/complexity_gate.py                        # the ratchet
python launcher/tools/complexity_gate.py --update-baseline      # record today's scores on purpose
python launcher/tools/complexity_gate.py --changed origin/main  # only files that changed - still needs the build above
```

Wired into CI as a step in `.github/workflows/build-diagnostics.yml`,
right after that job's own diagnostics build - the compile database it
depends on is that build's output, and a job in a different workflow file
cannot see another workflow run's checkout, so the gate cannot live
anywhere else. Not the pre-commit hook, which has to stay fast enough to
run on every commit. `--changed` is the fast path for local use instead:
point it at whatever ref the branch forked from.

clang-tidy is pinned to major 19, resolved the same way
`scripts/check-format.sh` resolves clang-format: `$CLANG_TIDY` if set,
then `clang-tidy-19` or `clang-tidy` on `PATH`, then ESP-IDF's bundled
esp-clang under `~/.espressif/tools/esp-clang/`. A different major scores
this check differently, so anything else is refused unless
`CLANG_TIDY_ANY_VERSION=1` is set - informational use only, never CI.
