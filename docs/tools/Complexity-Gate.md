# The cognitive-complexity ratchet

`launcher/tools/complexity_gate.py` replaces `cognitive_complexity.py`,
which parsed C with a regex that expected a function's opening brace alone
on its own line. PR #193 reformatted the tree to attach the brace to the
signature instead, so that regex stopped matching anything - reporting
zero functions, which reads exactly like a clean result, and nothing (no
hook, no CI) ever ran it to notice. `docs/sand/Sand-Simulation.md` still
cited the old tool's numbers for `sand_step()` and friends; they came from
states of the file that no longer exist, and this gate cannot reproduce
them - see that document's own "Why the liquid logic is its own file"
section for what it says now instead.

The new gate runs the real thing: clang-tidy's own
`readability-function-cognitive-complexity` check, already checked in as
`launcher/.clang-tidy` (added as a cross-check for the old script, unused
until now). clang-tidy parses a real AST - a reformat cannot blind it the
way it blinded a brace-position regex.

## The compilation database, and what it does not reach

clang-tidy needs real include paths and defines to parse a file at all.
Two databases exist in this tree already: `launcher/test/run_tests.sh`
knows the flags that compile the host-portable sources (proven by
compiling them every test run), and `idf.py clang-check` can emit one from
the full ESP-IDF build - but with Xtensa cross-compiler flags real clang
may not accept, and requires ESP-IDF's own multi-second environment
activation just to ask.

The gate takes the host route: `run_tests.sh --print-sources` and
`--print-flags` (two small early-exit branches added to that script) hand
back the exact file list and flags it already proves compile, and the gate
turns them into a `compile_commands.json` under `launcher/build.tidy/`
(gitignored, regenerated every run - nothing here is hand-maintained, so
the file list cannot drift from what `run_tests.sh` itself compiles the
way the two lists in `cognitive_complexity.py` and reality once did).

That determines coverage precisely:

**Measured** - every file in `run_tests.sh`'s own source list, minus
vendored code: the shell's portable modules (`input/`, `display/`, parts
of `util/` and `ui/`), every app's non-hardware logic (`sand.c`,
`sand_liquid.c`, `sand_reactions.c`, `sand_plants.c`, `sand_impulse.c`,
`material_palette.c`, cube's and diagnostics' portable files, ...), and
this project's own test suites (`test/suites/*.c`, each app's
`suite_*.c`) - 112 files, 862 functions as of this writing.

**Not measured** - anything that cannot compile for a host at all:

- every app's hardware-facing entry point: `app_sand.c`, `app_cube.c`,
  `app_diagnostics.c`
- the shell's own hardware-facing files: `main.c`, `gfx/gfx.c`,
  `ui/ui.c`, `ui/ui_launcher.c`, `board/board_esp32s3.c`,
  `boot/boot_anim.c`, `boot/post.c`, `boot/post_ui.c`, `boot/selftest.c`,
  `input/buttons.c`, `input/imu.c`, `input/touch.c`, `util/device_state.c`,
  `util/screenshot.c`
- `apps/*/tools/` sweep and report scripts - excluded by the same
  convention `main/CMakeLists.txt` and `run_tests.sh` already use,
  not something this gate added
- vendored code (`launcher/components/`, and the vendored Unity under
  `test/framework/`), excluded on purpose - a ratchet on this project's
  own functions has nothing to say about code it did not write
- a `static inline` helper defined only in a shared header (for example
  `sand_priv.h`'s `dest_row()`/`mark_rows()`) - clang-tidy's default
  scope is the file actually being compiled, not headers it pulls in, so
  a function that only ever lives in a header is invisible to this gate
  too. Stated here rather than silently accepted.

A gate that silently skips files is the exact failure being replaced, so
`complexity_gate.py` refuses to run quietly past either kind of gap: it
exits loudly if a scan finds zero functions at all, or (on a full scan)
finds fewer than half the baseline's function count - both are what
`cognitive_complexity.py`'s own failure would have looked like from the
outside.

## A ratchet, not a threshold

Some of what a full scan finds today is complex on purpose (`sand.c`'s
per-cell dispatch, `sand_reactions.c`'s chemistry). A fixed threshold
would either fail on all of it or be raised so high it catches nothing
new. Instead, `launcher/tools/complexity_baseline.txt` records every
measured function's current score, one line per function
(`score<TAB>file:line<TAB>name`, sorted worst first) - a plain text file
meant to be diffed in review like any other.

The gate compares today's scan against that file by **(path, function
name)**, never by line number - a function that only moved because
something above it grew is not a complexity change, and matching on line
would make it look like one. That choice also settles renames and
deletions without special-casing either:

- a function whose score **rose** above its baseline entry fails the gate
- a function whose score **fell** passes, with a note - lowering the
  baseline is only ever `--update-baseline`, run by a person on purpose,
  never automatic, so an improvement has to be noticed before it sticks
- a function with **no baseline entry** (genuinely new, or the new half
  of a rename) is judged against `NEW_FUNCTION_THRESHOLD` (15) instead -
  Sonar's own default line, and the same one
  `docs/sand/Sand-Simulation.md`'s "Broken down further" section says
  this project already drove every function in `main/` under by hand once
  before. Below it, silence; above it, the gate treats an unreviewed
  function scoring like `sand_step()` used to as worth a second look
  before it merges, not after
- a baseline entry with **no matching function** (the old half of a
  rename, or a real deletion) is reported as stale and otherwise ignored -
  it costs nothing to leave in the file until the next
  `--update-baseline` regenerates it

## Using it

```sh
python launcher/tools/complexity_gate.py                    # the ratchet (CI default)
python launcher/tools/complexity_gate.py --update-baseline  # record today's scores on purpose
python launcher/tools/complexity_gate.py --changed origin/main  # only files that changed - fast, local
```

Wired into CI in `.github/workflows/comment-rules.yml` as its own job (a
real clang-tidy pass over the whole measured set runs tens of seconds, not
the sub-second cost of the comment and doc checks beside it) - not into
the pre-commit hook, which has to stay fast enough to run on every commit.
`--changed` is the fast path for local use instead: point it at whatever
ref the branch forked from.

clang-tidy is pinned to major 19, resolved the same way
`scripts/check-format.sh` resolves clang-format: `$CLANG_TIDY` if set, then
`clang-tidy-19` or `clang-tidy` on `PATH`, then ESP-IDF's bundled
esp-clang under `~/.espressif/tools/esp-clang/`. A different major scores
this check differently, so anything else is refused unless
`CLANG_TIDY_ANY_VERSION=1` is set - informational use only, never CI.
