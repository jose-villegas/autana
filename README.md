# Autana

[![Host Tests](https://github.com/jose-villegas/autana/actions/workflows/host-tests.yml/badge.svg?branch=main)](https://github.com/jose-villegas/autana/actions/workflows/host-tests.yml)
[![Build (Release)](https://github.com/jose-villegas/autana/actions/workflows/build-release.yml/badge.svg?branch=main)](https://github.com/jose-villegas/autana/actions/workflows/build-release.yml)
[![Build (Diagnostics)](https://github.com/jose-villegas/autana/actions/workflows/build-diagnostics.yml/badge.svg?branch=main)](https://github.com/jose-villegas/autana/actions/workflows/build-diagnostics.yml)
[![Shell Scripts](https://github.com/jose-villegas/autana/actions/workflows/shell-scripts.yml/badge.svg?branch=main)](https://github.com/jose-villegas/autana/actions/workflows/shell-scripts.yml)
[![Format](https://github.com/jose-villegas/autana/actions/workflows/format.yml/badge.svg?branch=main)](https://github.com/jose-villegas/autana/actions/workflows/format.yml)

Autana is a small game engine for ESP32 AMOLED boards, growing out of
`launcher`, a custom app shell for the [Waveshare
ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/) board — dual-core
Xtensa LX7 @ 240 MHz, 8 MB octal PSRAM, a 368×448 AMOLED panel, capacitive
touch, and a 6-axis IMU. Everything here
drives the hardware directly rather than through a display framework: LVGL
ships as a transitive dependency of the board support package but is never
called, saving the internal RAM it would otherwise cost before drawing
anything.

## What's inside

A minimal shell (`launcher/main/`) that lists and switches between
self-contained apps, each living entirely in its own
`launcher/main/apps/<name>/` folder — adding or removing one touches no
other file. Currently:

- **Falling Sand** — a sandbox of powders, liquids, gases and fire
  chemistry, steered by tilting the board and poured with a touch. The most
  substantial piece of engineering in this repo: a flash-resident material
  system, a hybrid mass-diffusion water model, one impulse mechanism behind
  explosions, thrown chunks and water's own splash, and a device-verified
  performance budget for every hot path. See `docs/sand/Sand-Simulation.md`.
- **Render Lab** — software-rendering experiments, no GPU: a Gouraud-shaded
  rotating cube, wireframe primitives, and a ray-traced Cornell box.
- **Diagnostics** — a bench tool: a hardware self-test (POST) report plus a
  developer-toggles page; ships in any development build (`--dev` or the
  diagnostics build), never release. The on-device self-test *runner* on
  that page is narrower still — only the diagnostics build, the one that
  also carries the test suites.

A power-on self-test (`launcher/main/boot/post.c`) runs in every build, release included,
and checks storage, memory, sensors and the display on every boot.

## Setting up a clone

```bash
scripts/add-tools-to-path.sh           # puts `autana` on PATH - once per machine
scripts/install-git-hooks.sh           # pre-commit format check - once per clone
```

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.5+, and
its own export script has to work: `idf.py` cannot run under Git Bash, so on
Windows the build scripts hand that step to `cmd` and need a working
`export.bat`. They find it from `IDF_PATH`, which has to be set where they
run (Linux and macOS fall back to `~/esp/esp-idf`), and refuse to build
without it. Set `IDF_TOOLS_PATH` too whenever
the toolchain is not where ESP-IDF's installer puts it by default - that root
is also where the checks find the bundled clang-format and clang-tidy, and
where `autana` finds the Python that carries pyserial.

Host tests need a **host** compiler, not the ESP32 one:

| Platform | |
|---|---|
| Windows | `winget install BrechtSanders.WinLibs.POSIX.UCRT` |
| Debian/Ubuntu | `sudo apt install build-essential` |
| macOS | `xcode-select --install` |

The complexity gate, alone among the checks, also wants
`git submodule update --init` - see
[`docs/tools/Complexity-Gate.md`](docs/tools/Complexity-Gate.md).

## Quick start

```bash
cd launcher && idf.py build            # release — no test code, ships to the board
autana flash rel                       # flash it - works from any shell, including Git Bash
autana monitor                         # print what it says

./launcher/test/run_tests.sh           # host tests, portable suites - see Testing-Guide.md
autana selftest                        # builds the diagnostics variant, flashes it,
                                        # runs every suite on the actual chip
```

Everything that touches the board - flashing, the console, a suite run, a
screenshot - goes through the `autana` command, one terminal command
covering all of it, taking a device lock so two sessions never fight over
the port; see [`docs/tools/Autana-CLI.md`](docs/tools/Autana-CLI.md). For a
markdown report instead of a pass/fail line, use the wrappers below - `.sh`
scripts that write into their own `tools/results/`:

```bash
./launcher/tools/report_test_results.sh # every suite, pass/fail
./launcher/main/apps/sand/tools/report_performance.sh  # frame-budget numbers
```

`autana monitor` attaches to the console without paying ESP-IDF's ~90s
environment-activation cost on every call, and decodes any crash address it
sees against a build's `.elf`.

`autana screenshot` captures whatever the device currently has on screen to
a lossless `.png`, plus a same-named `.json` snapshot of device state at
that exact frame (sensors, memory, clock), over that same serial
connection - no SD card, no button on the device, just the running
firmware and a cable already plugged in. Needs neither `idf.py` nor
PowerShell.
Development-only (`autana flash dev` / `autana flash diag`) - a
release build carries none of it, see
[`docs/Build-Variants.md`](docs/Build-Variants.md).

## Documentation

Each doc earns its length — these are working notes from actually building
this, not a tour. Start wherever your question is:

| | |
|---|---|
| [`docs/Launcher-Architecture.md`](docs/Launcher-Architecture.md) | How the shell and its apps fit together; the three rules that shape everything; why the UI toolkit is microui, not LVGL. |
| [`docs/Building-an-App.md`](docs/Building-an-App.md) | Start here to write an app: the `app_t` endpoints, registration, the lifecycle the shell drives, and the folder convention. |
| [`docs/Gfx-and-Presentation.md`](docs/Gfx-and-Presentation.md) | How a draw call reaches the panel: the three draw targets, the dirty tracker, the present path, the band ring, heal. |
| [`docs/Text-and-Fonts.md`](docs/Text-and-Fonts.md) | The two kinds of font, the text calls, font roles, text in a microui screen, and how to add a typeface. |
| [`docs/sand/Sand-Simulation.md`](docs/sand/Sand-Simulation.md) | The falling-sand app in depth: materials, the water model, gas and fire chemistry, temperature, the two-core sweep, and the performance numbers behind every design choice. |
| [`docs/notes/`](docs/notes/README.md) | Board-specific hardware notes: the memory budget, panel and touch gotchas, flashing and recovery. Split by topic - start at the index. |
| [`docs/C-Style-Guide.md`](docs/C-Style-Guide.md) | The C style: what the formatter decides, what judgment decides, and how the pre-commit hook and CI keep the tree from drifting. |
| [`docs/Testing-Guide.md`](docs/Testing-Guide.md) | How the host and on-device test suites work, how to run one suite on the board, and how to make code testable. |
| [`docs/Build-Variants.md`](docs/Build-Variants.md) | What release, dev and diagnostics builds each carry: the Kconfig flags, the suite scope, and why release contains no test code. |
| [`docs/Building-a-Screen.md`](docs/Building-a-Screen.md) | Start here to build or change a UI screen: the loop, the house rules, and how to do what a screen needs. |
| [`docs/Autana-Rendering-Roadmap.md`](docs/Autana-Rendering-Roadmap.md) | Proposal: the order of investment for the rendering engine and its target games. |
| [`docs/sand/`](docs/sand/README.md) | The sand app's own doc set - architecture, materials, reactions, shading, testing. |
| [`docs/plans/`](docs/plans/README.md) | Designs for work not yet built, or built from a written plan. |
| [`docs/tools/`](docs/tools/README.md) | How the repository's checks and host-side tools work: the complexity gate, documentation drift, and the render harness. |

## Status

Actively developed, single-maintainer, not affiliated with Waveshare or
Espressif. Board-specific enough that most of this will not transfer
directly to other hardware, but the *reasoning* in the docs above — sweep
order in a cellular automaton, checking what memory tiers a chip actually
has before assuming them, how to keep test code out of a release image —
should.
