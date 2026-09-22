# Build variants

What a build is FOR is one Kconfig `choice` in `main/Kconfig.projbuild`, and
exactly one of its two entries is ever true:

- **`CONFIG_LAUNCHER_RELEASE`** — the product, and the default. No test
  suites, no profiling counters, no logging meant to be read over a serial
  console.
- **`CONFIG_LAUNCHER_DEVELOPMENT`** — everything meant for someone at the
  device or watching its console while working on it: frame timings, step
  counters, debug overlays, the screenshot listener, the Diagnostics app.

**`CONFIG_LAUNCHER_SELFTEST`** is a separate option on top, off by default,
that compiles the on-device test suites into the image. It *depends on*
`LAUNCHER_DEVELOPMENT` rather than turning it on, so a config asking for
SELFTEST alone silently gets neither; `sdkconfig.defaults.diag` sets both for
that reason. Two further options narrow a SELFTEST image:
`LAUNCHER_SELFTEST_AUTORUN` (the suites run at boot rather than on demand)
and the `LAUNCHER_SELFTEST_SCOPE_*` choice (which suites are compiled at
all).

That is three images, each with its own build directory so one never
reconfigures another:

| image | flags | built by | directory |
|---|---|---|---|
| release | neither | `idf.py build`, `autana flash rel` | `build/` |
| dev | DEVELOPMENT | `autana flash dev` | `build.dev/` |
| diagnostics | DEVELOPMENT + SELFTEST | `autana flash diag`, `autana selftest` | `build.diag/` |

---

## Release builds contain no test code

`CONFIG_LAUNCHER_SELFTEST` defaults **off**, and the CMake conditional leaves
the suites and the runner out of the build entirely — not `#ifdef`-ed out,
simply never compiled. `build/launcher.elf` (release) is neither DEVELOPMENT
nor SELFTEST, so **the Diagnostics app** is out of it too, for a related but
separate reason: it is gated on `CONFIG_LAUNCHER_DEVELOPMENT`, a strictly
broader flag than `CONFIG_LAUNCHER_SELFTEST` (see
[Building-an-App.md](Building-an-App.md#an-app-is-a-folder) and
`main/CMakeLists.txt`) — it also ships in a `--dev` build, which carries no
test suites at all.

Verified rather than assumed, by counting symbols in the two images:

```sh
xtensa-esp32s3-elf-nm build/launcher.elf      | grep -ci 'unity\|suite_\|selftest\|app_diagnostics'   # 0
xtensa-esp32s3-elf-nm build.diag/launcher.elf | grep -ci 'unity\|suite_\|selftest\|app_diagnostics'   # nonzero
```

`app_diagnostics` belongs in the same count as `unity`/`suite_`/`selftest`
not because the app is selftest-shaped — most of it is not — but because
release is neither DEVELOPMENT nor SELFTEST, so every one of those symbols is
absent from that image whichever of the two flags gates it.

That matters for more than size. The suites draw to the framebuffer and drive
the panel, which is fine in diagnostics and unacceptable in a product; and test
hooks in a shipped image are a liability rather than a feature.

The **Diagnostics app** is a bench tool: entering it re-runs POST, which
cycles the audio power rail and re-mounts the SD card live (both while the
display keeps running undisturbed, since neither shares its bus).
Reasonable while debugging, not something to leave reachable in a shipped
product — hence DEVELOPMENT, not left ungated. The boot POST still runs in
release — only this way *in* is compiled out. The self-test *runner* inside
Diagnostics (the button, its result line, the `selftest_run()` call) is
narrower still: gated on `CONFIG_LAUNCHER_SELFTEST` specifically, inside
`app_diagnostics.c`, because `selftest_run()` is not even a linkable symbol
outside a SELFTEST build (`boot/selftest.c` is only added to `app_srcs`
under `CONFIG_LAUNCHER_SELFTEST` — see `main/CMakeLists.txt`).

## A diagnostics build can be scoped

A diagnostics build compiles **every** suite. A full run takes tens of
minutes; most of it is the sand frame-budget suite. Perf-scoped compiles
only the suites and files the perf scope declares - see `main/CMakeLists.txt`
- so a sand performance capture, which reads a dozen rows out of the full
run, does not pay for every other suite too.

Scoping buys run time and build time, not memory: the framebuffer lives in
PSRAM, so the `.bss` a dropped suite takes with it frees nothing a capture
was short of. It does change the image's layout in the 32 KB instruction
cache, which is why a scoped capture's numbers compare only with other
scoped captures, never with an unscoped run.

`CONFIG_LAUNCHER_SELFTEST` says whether the suites are compiled in;
`CONFIG_LAUNCHER_SELFTEST_SCOPE_*` says **which**. Excluding a suite removes
its `.text` *and* its `.bss`, which is what buys the run time back.

| scope | fragment | carries | for |
|---|---|---|---|
| Full — the default | none | every suite, shell-owned and app-owned | every gate: `autana selftest`, `report_test_results.sh` |
| Perf | `sdkconfig.defaults.diag_perf` | the suites and files `main/CMakeLists.txt` declares for it | a sand frame-budget capture |

```sh
bash launcher/main/apps/sand/tools/report_performance.sh --perf-scope
# the image alone, left on the board, with no capture taken:
autana flash diag --perf-scope
# by hand, the fragment simply appends to the usual three:
idf.py -B build.diag.<yours> \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.diag;sdkconfig.defaults.diag_autorun;sdkconfig.defaults.diag_perf" \
  -D SDKCONFIG=build.diag.<yours>/sdkconfig build
```

Scoped around **what a run reads**, not around folders - the suites and
files the perf scope compiles are declared in the build files
(`main/CMakeLists.txt`), along with why the list is explicit rather than a
pattern and what fails loudly when it falls out of sync.

- **Full is the default and stays globbed.** A scope only ever narrows, and
  only when named, so coverage cannot shrink by accident.
- **The scenes suite comes along because its builders do,** and its own tests
  then check that the scenes the perf rows measure are still the scenes they
  claim to be.
- **Release is untouched.** Both scope symbols live under `LAUNCHER_SELFTEST`,
  itself under `LAUNCHER_DEVELOPMENT`; a release config resolves neither, and
  the suites were never in that image to scope.

A perf-scoped build is **not a gate**: it drops behaviour coverage on purpose.
Never take a merge decision from one, and never diff its numbers against an
unscoped capture's — different scope, different layout.

## Development-only instrumentation is its own flag, not SELFTEST

`CONFIG_LAUNCHER_SELFTEST` answers "does this build carry the test suites."
It does not answer "is this a development build" — that is a broader
question, and `CONFIG_LAUNCHER_DEVELOPMENT` answers it instead.

This project does not do telemetry. Nobody downstream ever reads a frame
counter or a step-timing average; the only audience for that kind of number
is a developer at the device or watching its serial console while working on
it. So anything built purely for that audience — rolling averages, per-frame
timers, a summary logged on exit — is pure cost in a release image: flash for
the strings and the accounting, cycles for the bookkeeping, for output that
helps nobody. It gets guarded by `CONFIG_LAUNCHER_DEVELOPMENT`, the same way
test code is guarded by `CONFIG_LAUNCHER_SELFTEST` — see `app_sand.c`'s frame
timing for the pattern.

The two are related but not the same flag, because they answer different
questions and can genuinely diverge:

- `LAUNCHER_SELFTEST` **depends on** `LAUNCHER_DEVELOPMENT` — a build
  carrying the test suites is a development build by definition. Kconfig does
  not turn the second one on for you: a config that asks for SELFTEST alone
  silently gets neither, which is why every defaults fragment sets both.
- The reverse is not forced. A build can want the profiling and logging
  without the test suites — watching real frame timings without also paying
  for Unity and the suites' own footprint.

`LAUNCHER_DEVELOPMENT` and `LAUNCHER_RELEASE` are the two entries of one
Kconfig `choice` (`main/Kconfig.projbuild`), so exactly one is ever true and
neither is "off by omission." Checking `CONFIG_LAUNCHER_DEVELOPMENT` means
"not a release build," not "development, or maybe some other thing nobody
named yet."

**The rule going forward:** guard anything whose only reader is a developer —
a log line, a rolling average, a debug overlay — with
`CONFIG_LAUNCHER_DEVELOPMENT`. Guard the test suites themselves, and anything
that only makes sense alongside them, with `CONFIG_LAUNCHER_SELFTEST`. Neither
belongs ungated, and neither belongs gated on the other one just because they
currently happen to travel together in `build.diag/`.

A bare log line specifically has a second, complementary mechanism worth
knowing about: ESP-IDF's own `CONFIG_LOG_MAXIMUM_LEVEL` compiles
`ESP_LOGI`/`ESP_LOGW`/etc. calls out of the binary entirely above a given
severity, project-wide, with no per-call-site `#if` needed — this project
just doesn't split that ceiling per build variant yet. See
[Log-Level-Plan.md](plans/Log-Level-Plan.md).

A FRAME_COST bracket (`util/frame_cost.h`) names a stage of the frame -
something the shell or a screen does once per frame - and stays in the
source, compiled out of release the same as everything else in this section.
A bracket put inside a stage to answer one question is scaffolding instead:
it leaves with the measurement it was for, the same as any switch that turns
a phase of work off to see what it cost. The first kind may stay because it
only reads a clock - the code that ships is the code that was measured,
minus two clock reads per stage.

## The Kconfig trap in REQUIRES

One thing must **not** be gated on `CONFIG_LAUNCHER_SELFTEST`: the `unity`
entry in `REQUIRES`.

ESP-IDF expands component requirements in an early pass where `CONFIG_*` is not
yet defined, so a Kconfig-gated `REQUIRES` silently evaluates false and does
nothing. `SRCS` and `target_compile_definitions` are evaluated in a later pass
and *do* work — which makes the failure genuinely confusing: the test sources
get compiled, `DEVICE_BUILD` is defined, and every one of them fails with
`fatal error: unity.h: No such file or directory`.

Worse, it only shows up on a **clean** build directory. An incremental build
already has a `sdkconfig`, so it appears to work — meaning this can sit latent
until CI, or until someone deletes `build.diag/`.

So `unity` is listed unconditionally. That costs release nothing: IDF puts unity
in the component graph either way, `REQUIRES` only decides whether `main` can
see its headers, and with no test sources compiled nothing references it and
`--gc-sections` drops it. Confirmed — the release binary is byte-for-byte the
same size with and without the entry.

```sh
idf.py build                          # build/       release, no test code
autana selftest                       # build.diag/  firmware + suites
```

The two use separate build directories so each keeps its own `sdkconfig` and
running the tests can never silently reconfigure your normal build.

This is the norm, not a compromise. Unit tests verify *units* - `touch_fsm.c`
compiles from identical sources with identical flags in both variants, and
linking a test framework beside it cannot change how it behaves. Verifying an
*image* is a separate activity (POST, functional tests, checksums) that unit
tests were never doing in either build.

If anything the direction favours release: the diagnostics variant carries more
code and less free RAM, so a suite passing there leaves release with more
headroom, not less.

A failing self test is logged, not fatal — the harness reads the result from
the console, and a board that still boots is easier to investigate than one
that refuses to.

---

## Related

- [`Testing-Guide.md`](Testing-Guide.md) — the suites these flags carry or
  drop, and how to run them.
- [`notes/Diagnostics-and-Debugging.md`](notes/Diagnostics-and-Debugging.md) —
  which tool to reach for when the board misbehaves, and which build each
  one needs.
- [`plans/Log-Level-Plan.md`](plans/Log-Level-Plan.md) — a planned
  compile-time log-severity ceiling per variant, complementing this split.
- [`plans/Settings-App-Plan.md`](plans/Settings-App-Plan.md) — the remaining
  DEVELOPMENT/SELFTEST seam in the Diagnostics app.
