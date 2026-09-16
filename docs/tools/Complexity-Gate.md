# The cognitive-complexity ratchet

`launcher/tools/complexity_gate.py` measures cognitive complexity with
clang-tidy's `readability-function-cognitive-complexity` check
(`launcher/.clang-tidy` holds its configuration) and ratchets every
first-party function's score against a committed baseline instead of a
fixed threshold.

## The compilation database

clang-tidy needs real include paths and defines to parse a file. The gate
gets these from `launcher/test/run_tests.sh --print-sources` and
`--print-flags`, which report the exact file list and flags that script
compiles for the host test build, and turns them into a
`compile_commands.json` under `launcher/build.tidy/` (gitignored,
regenerated on every run). Deriving the database from `run_tests.sh`
itself, rather than keeping a second copy of its file list, is what keeps
the gate's coverage identical to the host build's: whatever `run_tests.sh`
compiles is what the gate measures, with nothing to fall out of step.

**Measured** - every file in `run_tests.sh`'s own source list, minus
vendored code: the shell's portable modules (`input/`, `display/`, parts
of `util/` and `ui/`), every app's non-hardware logic (`sand.c`,
`sand_liquid.c`, `sand_reactions.c`, `sand_plants.c`, `sand_impulse.c`,
`material_palette.c`, cube's and diagnostics' portable files, ...), and
this project's own test suites (`test/suites/*.c`, each app's
`suite_*.c`).

**Not measured** - anything that cannot compile for a host at all:

- every app's hardware-facing entry point: `app_sand.c`, `app_cube.c`,
  `app_diagnostics.c`
- the shell's own hardware-facing files: `main.c`, `gfx/gfx.c`,
  `ui/ui.c`, `ui/ui_launcher.c`, `board/board_esp32s3.c`,
  `boot/boot_anim.c`, `boot/post.c`, `boot/post_ui.c`, `boot/selftest.c`,
  `input/buttons.c`, `input/imu.c`, `input/touch.c`, `util/device_state.c`,
  `util/screenshot.c`
- `apps/*/tools/` sweep and report scripts - excluded by the same
  convention `main/CMakeLists.txt` and `run_tests.sh` already use
- vendored code (`launcher/components/`, and the vendored Unity under
  `test/framework/`) - a ratchet on this project's own functions has
  nothing to say about code it did not write
- a `static inline` helper defined only in a shared header (for example
  `sand_priv.h`'s `dest_row()`/`mark_rows()`) - clang-tidy's default scope
  is the file actually being compiled, not headers it pulls in, so a
  function that only ever lives in a header is invisible to this gate

The gate fails loudly rather than passing quietly past a measurement gap:
a scan that finds zero functions, or (on a full scan) fewer than half the
baseline's function count, fails immediately instead of being reported as
a clean result.

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

```sh
python launcher/tools/complexity_gate.py                       # the ratchet (CI default)
python launcher/tools/complexity_gate.py --update-baseline     # record today's scores on purpose
python launcher/tools/complexity_gate.py --changed origin/main # only files that changed - fast, local
```

Wired into CI in `.github/workflows/comment-rules.yml` as its own job (a
real clang-tidy pass over the whole measured set runs tens of seconds, not
the sub-second cost of the comment and doc checks beside it) - not into
the pre-commit hook, which has to stay fast enough to run on every commit.
`--changed` is the fast path for local use instead: point it at whatever
ref the branch forked from.

clang-tidy is pinned to major 19, resolved the same way
`scripts/check-format.sh` resolves clang-format: `$CLANG_TIDY` if set,
then `clang-tidy-19` or `clang-tidy` on `PATH`, then ESP-IDF's bundled
esp-clang under `~/.espressif/tools/esp-clang/`. A different major scores
this check differently, so anything else is refused unless
`CLANG_TIDY_ANY_VERSION=1` is set - informational use only, never CI.
