# Host Tests

The portable suites, compiled and run on your own machine. This is the
everyday red-green loop: the tests take seconds, the wait is compiling.
Back to the [Testing Guide](README.md).

## Install what you need

You need a **host** C compiler (not the ESP32 toolchain), GNU make, CMake
(it lists the engine sources) and Python 3. ESP-IDF and a board are not needed.

| Platform | Install |
|---|---|
| Windows | [Git for Windows](https://git-scm.com/download/win) for Git Bash, then `winget install BrechtSanders.WinLibs.POSIX.UCRT` (compiler and `mingw32-make`) and `winget install Kitware.CMake Python.Python.3.12` |
| Debian/Ubuntu | `sudo apt install build-essential cmake python3` |

Open **Git Bash** (not PowerShell) for the commands in these docs; reopen it
after installing so it sees the new `PATH`.

`run_tests.sh` is POSIX sh: Git Bash or MSYS on Windows, native on Linux. It
looks for a compiler in `$CC`, then `PATH`, then where winget puts MinGW, and
says how to install one if none is found.

The `.mjs` suites additionally need Node.js (`winget install OpenJS.NodeJS.LTS`,
or `sudo apt install nodejs`). The
scripts try `python3`, `python`, then `py`; `python` in these docs means
`python3` there.

## Run them

A first cold build compiles everything; reruns take seconds. A good run ends
with Unity's summary:

```
... Tests 0 Failures 0 Ignored
OK
```

```sh
./launcher/test/run_tests.sh                  # portable suites, on this machine
./launcher/test/run_tests.sh --verbose        # the full build-and-test stream
./launcher/test/run_tests.sh --sanitize       # with UBSan, and ASan on Linux (CI too)
./launcher/test/run_tests.sh --build-dir DIR  # build in DIR, so two runs never share a binary
```

The default output is the verdict and Unity's test counts. A failure adds the
failing assertion lines and the first compiler and gate errors, including the
stack check. The full stream is always saved to
`launcher/test/build/run_tests.log`; the path prints before the run.

| Flag | Does |
|---|---|
| `--jobs N` | compile jobs; default half the CPUs, at most 8 |
| `--build-only` | compile, link and run the stack gate; skip the suites |
| `--sanitize` | UBSan everywhere, ASan on Linux; any finding fails the run |

`--sanitize` links a probe program first. With the UBSan runtime, findings are
reported and the run continues. Without it (WinLibs GCC on Windows) the build
uses `-fsanitize-undefined-trap-on-error`: undefined behaviour stops the
process and the failure summary names the active test.

### Incremental builds

`run_tests.sh` writes a Makefile into `launcher/test/build/` and lets GNU make
(`make`, or `mingw32-make` beside WinLibs) compile one object per source, from
the compiler's own dependency files.

- A rerun compiles nothing.
- A touched `.c` recompiles one object; a touched header recompiles exactly its includers.
- Every flag is stamped, so a changed flag rebuilds everything. A sanitizer build keeps its own object directory.

### Git hooks

`scripts/install-git-hooks.sh` installs the local hooks. Pre-push runs the
workflow step marked `PUSH_TEST_GATE: "true"` (here `run_tests.sh --sanitize`)
once when a pushed range changes C or C++ source, and refuses the push on failure.

- Run the same command by hand to diagnose it; `git push --no-verify` skips the hook.
- The hook uses the scripts of the current worktree.
- A new remote branch is compared against its merge base with `origin/main`, or the empty tree when that ref is missing.

## The host enforces the device's memory limits

*Reference: skip this section until a host run fails with a stack or heap
message, or you want to know why one can.*

The host has megabytes of stack and gigabytes of heap; the board does not.
The real figures live in `launcher/tools/device/device_profiles/esp32s3.sh`
(`DP_MAIN_TASK_STACK_BYTES`, `DP_FREE_HEAP_BYTES`,
`DP_LARGEST_FREE_BLOCK_BYTES`), chosen by `$DEVICE_PROFILE`. A second board
is a new profile, not an edit everywhere; a never-measured field is the
literal `unmeasured`, and the loaders refuse to hand it to a gate.

The host catches three bug classes that would otherwise cost a
build-flash-capture cycle each:

| Bug | Host check |
|---|---|
| A fixture whose stack frame cannot fit | `-fstack-usage` rebuild; `check_stack_usage.py` fails any frame over the profile ceiling. Frames already over are listed as debt (`PRE_EXISTING_STACK_DEBT`) so a new one still fails. |
| A fixture that allocates more than the board has | `malloc`/`calloc`/`realloc`/`free` are wrapped (`-Wl,--wrap=`) into `heap_arena.c`, a first-fit arena of exactly the device's free heap. First-fit with coalescing, because contiguity is what bites: a 38 KB largest block fails a tens-of-KB request even with 50 KB free. |
| An allocation a test does not release | Outstanding-block count is snapshotted per test across internal and PSRAM pools; a leftover fails the test with byte totals. |

The same audit exists on the device: it records the 8-bit and 32-bit free heap
before every test and, after a first-use drop, reruns the test and checks the
second run at zero bytes. `leaked ... heap on its second run` means the loss
repeats. The audit holds however a test ends, `TEST_PASS()` included.

On Linux, `--sanitize` poisons freed arena blocks, so ASan catches
use-after-free and out-of-bounds access in arena memory. Arena memory is
static storage, so it is not reported as a process leak.

The framebuffer lives in PSRAM, not internal DRAM, so it does not compete
with an app's internal allocations. Nothing gates internal-heap headroom at
build time; watching it (the free-heap figure in a profile, `HEAPMARK` boot
lines on a dev build) is a manual habit.

### Where the approximations end

The arena models one process from a clean start, so it cannot show
fragmentation inherited from a real boot. The stack gate reads the host
compiler's frames one function at a time; `check_stack_usage_device.sh`
rechecks with the target compiler when a host frame nears the ceiling.
Neither replaces a device capture. They make a class of bug cost a second on
a laptop instead of a capture cycle.

### The stack chain gate

`launcher/tools/quality/stack_chain_gate.py` follows call chains from named
roots and checks the deepest path against the stack budget. It runs in
self-test CI and in `autana build diag --check`, without flashing.

It discovers `stack_chain.txt` files under `launcher/` (build directories
excluded). Apps put roots in their own spec, the engine in the test spec.

| Line | Meaning |
|---|---|
| `root FUNCTION KIND` | an entry point; `KIND` is `test`, `frame`, `system` or `boot` |
| `indirect CALLER... : CALLEE...` | the targets of a function pointer |

Rules:

- Every spec feeds one graph. Declare each indirect edge once, beside the caller; engine edges go in the engine spec.
- An undeclared pointer caller reachable below a root fails. Private names may be written `file.c:function`.
- Suite registrations and Unity's wrapper supply the runner's pointer edges themselves.
- Runner overhead is the deepest measured path from the main-task entry to the kind's endpoint: `call_protected`, `shell_step_app`, `scene_shell_render` or `app_boot_init`. The boot root is `boot_anim_run`.
- Missing paths and non-static frames fail.

Every root has the budget `DP_MAIN_TASK_STACK_BYTES` minus `timing.c`'s
reserve, the derived runner frames and the target context
(`XT_STK_FRMSZ`, the aligned `XT_CP_SIZE`, TLS from the ELF). Interrupt
context includes window spills. Worker-task calls count separately; a
dispatcher's synchronous fallback stays on its caller's stack.

The diagnostics build writes each object's call graph (`.ci`) and frame
sizes (`.su`) beside it. The gate reads the engine, test, package and app
ones, and those of the IDF files defining `main_task`, `vPortTaskWrapper`
and `UnityDefaultTestRun`.

Only frames in those files count: the C library's printf family, `esp_log` and
FreeRTOS are not. `timing.c`'s board
stack-watermark check covers that remainder. The maximum is conservative; a
device's high-water mark may never meet the clipping paths.

## Host render scenes

`launcher/tools/render/render_all_scenes.sh` renders every declared screen
on the host and pins its pixels. It is for layout and regression checks and
**never a perf oracle**. The manual is
[`tools/Render-Harness.md`](../tools/Render-Harness.md); the render check also
runs the [frame watch gate](Performance-and-Budgets.md#the-frame-watch-as-a-gate).
