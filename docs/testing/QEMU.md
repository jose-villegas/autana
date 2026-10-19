# QEMU: the device image with no board

Espressif's QEMU has an `esp32s3` machine. The diagnostics image runs its
suites there: the real Xtensa binary, ESP-IDF, FreeRTOS and both cores, with
no board and no port to share. Any number of instances run at once.
Back to the [Testing Guide](../Testing-Guide.md).

## Run it

```sh
. launcher/tools/build/idf.sh
idf_init "$PWD/launcher" "$PWD/launcher/tools/build"
idf_in "$PWD/launcher" python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa # once
./launcher/test/run_qemu_tests.sh --perf-scope             # build + run
./launcher/test/run_qemu_tests.sh --suite suite_job --screenshot shot.png
```

The last form is the everyday one. It builds the image without autorun
(`build.qemu.shell/`), boots into the shell, then speaks the board's console
protocol: `runsuite <name>` per `--suite`, waited out to `RUNSUITE_COMPLETE`,
then `screenshot`, decoded to a PNG and a `.json` state.

| | Time |
|---|---|
| Boot, one suite, one capture | about 1.5 minutes |
| A whole autorun | 5 to 9 minutes |
| Full scope to `SELFTEST_COMPLETE` | about 14 minutes on an idle desktop; twice that with `--icount` |

The screenshot is the firmware's real framebuffer, so it is a board-free way
to look at a screen, and the second backend the
[render harness](../tools/Render-Harness.md) diffs a host render against.

## What the image is

The autorun diagnostics build with `launcher/sdkconfig.defaults.qemu` layered
last, in its own `build.qemu*/`. That fragment:

- drops the 120 MHz flash setting, which QEMU's flash model cannot follow (such an image resets silently in the second-stage bootloader);
- moves the console to UART0, the port QEMU exposes (the runner reaches it as a local TCP socket);
- sets `CONFIG_LAUNCHER_QEMU`. No panel, I/O expander or touch controller exists, so `gfx_present.c` gives the unidentified board a null panel (`gfx_null_panel_device.c`).

The null panel keeps what the code depends on: a strip occupies the bus for
its transfer time at the current panel clock, one after another. So bus time
keeps its order (a window cheaper than a band, a band cheaper than a frame,
nothing sent costing nothing). A whole present's measured cost does not,
because the emulator prices the CPU work. A test that compares two presents'
times reports the comparison there instead of enforcing it.

Other missing hardware is stood in for:

| Hardware | In QEMU |
|---|---|
| Touch | `touch_inject()` leaves a sample the polling task reads; it still travels the touch state machine to `touch_read()` |
| IMU | `imu_init()` succeeds; `imu_read()` returns what `imu_inject()` last set, upright and still by default (the shell picks portrait) |
| Temperature | the read reports failure; ESP-IDF's driver would wait forever |

## Drive the shell

The console also accepts `TOUCH <down|up> <x> <y>` (panel pixels) and
`IMU <ax> <ay> <az>` (raw counts, 4096 to the g). `qemu_run.py --do` strings
them into what a user does, one step at a time. Its `screenshot`, `tap` and
`swipe` use the default `autana screenshot` view's pixels; `touch` and
`--touch` stay in panel pixels; `send` writes a console line as typed.

```sh
python launcher/test/qemu_run.py launcher/build.qemu.shell \
  --do "tap 95 187" --do "wait 2500" --do "screenshot app.png" \
  --do "tilt 0 -4096 0" --do "wait 2500" --do "screenshot landscape.png" \
  --do "swipe 222 1 222 200" --do "screenshot home.png"
```

That opens an app, turns the board on its side and swipes home in about a
minute: boot animation, frame loop, launcher, enter/leave and rotation, none
of which a suite reaches. Leave `--icount` off here; a press is timed in the
emulated clock, which then runs far slower than the host's. An app without
`home_gesture` ignores the swipe, as on the board; `--do "send home"` returns
directly.

## What a run is evidence of

**A crash fails the run.** After every step the image must answer `BUILDID`
within `HEARTBEAT_S` (`qemu_run.py`), or the remaining steps are skipped. Any
run fails on a crash line (panic, `abort()`, failed `assert`) or a second
boot banner, whatever its tests reported. Board captures give the same
verdict through `crash_signs()` in `scripts/lib/device_capture.py`.

A run is evidence of pass and fail for any test that does not read a clock.
A measured time against a ceiling pegged on the board is reported, not
enforced. Tests of hardware QEMU lacks skip themselves (the performance
monitor, since every counter reads zero; a touch controller physically
answering), so any failure that remains is real and worth a board look.
A run says nothing about the real panel, touch controller, IMU or timing.

Without `--icount` each core gets a host thread, so a busy host stretches
two-core tests by minutes.

## Counting instructions: `--icount`

`--icount` puts both emulated cores on one host thread and advances virtual
time one nanosecond per instruction (`sleep=off`, so guest timers follow
instructions). A `us per step` line times 1000 is then instructions per step.

- **Valid** for a one-core step that sends nothing; time on the null panel's modelled bus passes with no instructions behind it. Captures of such a step differ by far less than the ratchet tolerance.
- **Invalid** for a step shared between two cores. The count sums both, so the idle core is charged for its spin or idle task. That floor barely moves between full work and almost none, and under host load moved up to 25%. Measure the same work on one thread and take the two-core verdict from the board.

The count says whether a change removed work, not whether it removed time on
this chip (see [`notes/Debugging.md`](../notes/Debugging.md#performance-seems-off)).
The guarded kernels and their gate are in
[Performance and budgets](Performance-and-Budgets.md#hot-kernel-instruction-ratchet).

## Parallel runs

Each `qemu_run.py --workdir` holds its own flash image, eFuse file and console
log; the build directory is only read. `run_qemu_tests.sh --build-only` builds
once for a driver that launches several. How many at once depends on whose
desktop it is, not on the runner.

## A QEMU with its bugs patched

Espressif's prebuilt QEMU has races that fail a correct image, for example
`LoadStorePIFAddrError` on an ordinary register access
([espressif/qemu#174](https://github.com/espressif/qemu/issues/174)). Each fix
is a file in `launcher/test/qemu/patches/` naming its upstream issue.
`patched_qemu.py` rebuilds the QEMU version ESP-IDF installs with them into
`launcher/build.qemu-xtensa/`; `qemu_run.py` uses it when present and prints
which QEMU it started. CI builds it once per version and patch set, cached.

| Host | QEMU a run uses |
|---|---|
| CI, and Linux after `patched_qemu.py build` | patched |
| Linux before that, and Windows | Espressif's prebuilt: a crash there can be QEMU's |

```sh
sudo apt-get install $(python3 launcher/test/qemu/patched_qemu.py deps)   # once
python3 launcher/test/qemu/patched_qemu.py build                          # minutes, once per QEMU version
```
