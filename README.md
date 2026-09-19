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

A minimal shell (`main/`) that lists and switches between self-contained
apps, each living entirely in its own `main/apps/<name>/` folder — adding or
removing one touches no other file. Currently:

- **Falling Sand** — a cellular-automaton sandbox with sand, water and
  stone, steered by tilting the board and poured with a touch. The most
  substantial piece of engineering in this repo: a flash-resident material
  system, a hybrid mass-diffusion water model, one impulse mechanism behind
  explosions, thrown chunks and water's own splash, and a device-verified
  performance budget for every hot path. See `docs/sand/Sand-Simulation.md`.
- **3D Cube** — a Gouraud-shaded software rasterizer, no GPU.
- **Diagnostics** — a bench tool: a hardware self-test (POST) report plus a
  developer-toggles page; ships in any development build (`--dev` or the
  diagnostics build), never release. The on-device self-test *runner* on
  that page is narrower still — only the diagnostics build, the one that
  also carries the test suites.

A power-on self-test (`launcher/main/boot/post.c`) runs in every build, release included,
and checks storage, memory, sensors and the display on every boot.

## Quick start

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.5+.

```bash
cd launcher && idf.py build            # release — no test code, ships to the board
idf.py -p <PORT> flash monitor

./launcher/test/run_tests.sh           # host tests, portable suites, <1 s
./launcher/test/run_device_tests.sh    # builds the diagnostics variant, flashes it,
                                        # runs every suite on the actual chip
```

`idf.py` cannot run under Git Bash, so on Windows use the wrappers below -
`.sh` scripts that reach ESP-IDF through `launcher/tools/idf.sh` and write a
markdown report into their own `tools/results/`:

```bash
./launcher/tools/build_flash.sh        # build + flash the release firmware
./launcher/tools/report_test_results.sh # every suite, pass/fail
./launcher/main/apps/sand/tools/report_performance.sh  # frame-budget numbers
```

`./monitor.sh` (repo root) attaches to the console without paying ESP-IDF's
~90s environment-activation cost on every call.

`./launcher/tools/screenshot.sh` captures whatever the device currently has
on screen to a lossless `.png`, plus a same-named `.json` snapshot of
device state at that exact frame (sensors, memory, clock), over that same
serial connection - no SD card, no button on the device, just the running
firmware and a cable already plugged in. Needs neither `idf.py` nor
PowerShell. Development-only (`build_flash_dev.sh` / `build_flash.sh --diag`)
- a release build carries none of it, see
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
