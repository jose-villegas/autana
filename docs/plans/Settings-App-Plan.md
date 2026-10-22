# Plan: Settings and SELFTEST/diagnostics naming

**Status**: Step 1's first milestone is built: Settings shows DISPLAY in every
build and DEVELOPER in development builds. Orientation and the self-test
runner remain in Diagnostics. The remaining milestones and step 2 are open.

## Current ownership

Settings is a view of values held by display and gfx. It reads a fresh
snapshot each frame and calls an owner's setter only when a control changes.
It has no settings store of its own.

| Section | Availability | Owner and lifetime |
|---|---|---|
| DISPLAY: panel clock | Every build | Display persists the system value in NVS and restores it on app switches. |
| DEVELOPER: panel-grid overlay, leaf-rect overlay, interlace, send audit | Development builds | Gfx holds runtime values, reset at boot. |

The panel-clock row retains its warning about stray pixels and thin lines.
The fast clock is outside the panel's rating; see
[Display-and-Rendering.md](../notes/Display-and-Rendering.md).

Diagnostics is development-only. It shows the POST report and orientation
readout; the self-test button and result line additionally require
`CONFIG_LAUNCHER_SELFTEST`.

## Step 1: remaining milestones

- Decide where the orientation readout belongs.
- Decide whether developer rows need an unlock gesture.
- Design backdrop and memory views before adding their controls.

A memory view should show the allocation ledger beside display-mode choices:
framebuffer, static data, free heap and largest contiguous block, plus each
choice's projected change. Resolution and colour mode need that context.

## Step 2: unify SELFTEST/diagnostics naming

`CONFIG_LAUNCHER_SELFTEST`, `selftest/selftest.c` and `selftest_run()` name the
suite runner. The diagnostics build directory, CLI variant and CI workflow
use the other word. Development instrumentation has its own wider gate.

| Decision | Affected surface |
|---|---|
| Choose selftest or diagnostics for the suite variant | Kconfig, build directories, CLI flags, workflow and docs |
| Decide whether autorun follows the same rename | `CONFIG_LAUNCHER_SELFTEST_AUTORUN` |
| Rename the CI workflow if required | Badge URLs and workflow references |

The naming work remains separate from UI changes.
