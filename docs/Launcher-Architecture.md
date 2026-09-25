# Launcher Architecture

How the shell, the apps and the screen fit together, and why ownership is
arranged this way. Read this before changing the frame loop. To write an app,
start at [Building-an-App.md](Building-an-App.md).

---

## Layout

```
launcher/
├── bootloader_components/  hooks built into the second-stage bootloader
│   └── pmic_cold_boot/     a warm reset becomes a PMIC power cycle at 120 MHz
├── components/
│   ├── esp32_s3_touch_amoled_1_8/  Waveshare BSP, LVGL trimmed
│   ├── microui/        MIT, patched for this chip (see below)
│   └── small3dlib/     CC0, header-only
├── tools/              generators, build/flash wrappers, report scripts
│   ├── gen_zeta_curve.py       generates main/boot/boot_anim_curve.h
│   ├── gen_boot_anim_timeline.py, gen_boot_anim_image.py,
│   │                           gen_gfx_palette_standard.py, gen_icons.py
│   ├── gen_ui_layout.py        bakes main/ui/<screen>_layout.json into its header
│   ├── gen_ridge_curve.py      bakes design/boot/ridge.png into main/ui/ridge_curve_generated.h
│   ├── build_flash.sh          build + flash; --dev and --diag variants
│   ├── device_report.sh        the one build-flash-capture-report path
│   └── report_test_results.sh  every suite, pass/fail
├── test/               the host runner and the shell's own suites
└── main/
    ├── main.c          the frame loop and app switching
    ├── app.h           the shell/app contract
    ├── app_registry.c  the registered apps, sorted by name (host-tested)
    ├── boot/           runs once each, before the frame loop exists
    │   ├── post.{h,c}          power-on self test
    │   ├── post_layout.{h,c}   the POST report's geometry   (host-tested)
    │   ├── post_ui.{h,c}       the POST report, on screen
    │   ├── selftest.{h,c}      runs the suites at boot (diagnostics build)
    │   ├── boot_anim.{h,c}     the startup animation  (the .h is host-tested)
    │   ├── boot_anim_curve.h   GENERATED - see tools/gen/gen_zeta_curve.py
    │   ├── boot_anim_image.h   GENERATED - see tools/gen/gen_boot_anim_image.py
    │   └── boot_anim_timeline.h GENERATED - from boot_anim_timeline.json
    ├── render/         3D transform, clip and projection shared by boot and apps
    │   ├── r3d_project.h       camera-space near clip, perspective (host-tested)
    │   ├── r3d_camera.h        camera description, upright roll, viewport fit (host-tested)
    │   └── r3d_ray.h           float ray camera for a tracer        (host-tested)
    ├── board/          the one board's pins and peripherals
    │   ├── board.h             what any board must provide
    │   └── board_esp32s3.c     this board's answer
    ├── display/        the panel behind the framebuffer
    │   ├── display.{h,c}       which way is up, with hysteresis  (host-tested)
    │   └── panel_clock.{h,c}   pixel-clock resolution       (host-tested)
    ├── gfx/            the one framebuffer, and what draws into it
    │   ├── gfx.{h,c}           owns THE framebuffer, primitives, text
    │   ├── gfx_mode.h          the mode-grant arithmetic  (host-tested)
    │   ├── gfx_band.h          the band-ring state machine (host-tested)
    │   ├── gfx_color.h         what a pixel is            (host-tested)
    │   ├── gfx_dirty.h         which bands changed        (host-tested)
    │   ├── gfx_indexed.h       paletted pixels            (host-tested)
    │   ├── gfx_palette*.{h,c}  the standard palette       (host-tested)
    │   ├── gfx_target.h        where a draw call lands    (host-tested)
    │   ├── gfx_null_panel.{h,c}  a panel with nothing behind it, for QEMU
    │   ├── gfx_glow.h          a curve drawn as light     (host-tested)
    │   ├── gfx_full_redraw.h   when everything must repaint (host-tested)
    │   ├── gfx_heal.h          repairing a torn band      (host-tested)
    │   ├── gfx_fb_guard.h, gfx_present_guard.h  misuse traps (host-tested)
    │   ├── gfx_font.h          what a font IS             (host-tested)
    │   ├── font8x8_basic.h     the built-in 8x8 glyphs
    │   ├── gfx_font_roles.h    which font plays which part (host-tested)
    │   └── icon.h, icons_system.h  artwork no font provides (host-tested)
    ├── ui/             microui integration, shared by the shell and apps
    │   ├── ui.{h,c}, ui_internal.h, ui_build.c
    │   ├── ui_pointer.{h,c}    input_t -> move/down/up events   (host-tested)
    │   ├── ui_slider.h         geometry for an integer slider   (host-tested)
    │   ├── ui_style.h          how a control's frame looks (host-tested)
    │   ├── ui_transform.h      the quarter-turn mapping     (host-tested)
    │   ├── ui_anchor.h         a rect placed against an edge (host-tested)
    │   ├── ui_scroll.{h,c}     a screen with more rows than fit (host-tested)
    │   ├── ridge_pose.h        the ridge's pose from gravity (host-tested)
    │   ├── ui_launcher.{h,c}, ui_launcher_draw.c   the home screen
    │   ├── ui_control_center.{h,c}, ui_control_center_draw.c
    │   │                       Control Center, over a dimmed home screen
    │   ├── control_center_layout.json, control_center_layout_generated.h
    │   │                       its authored rects, and the baked table
    │   ├── ui_ridge.{h,c}      the launcher's backdrop: the ridge, level with the
    │   │                       horizon, waved by touch and shaking
    │   ├── ridge_motion.h      its breathing, its wave, the wave's momentum (host-tested)
    │   ├── system_navigation.{h,c}  which system screen is up (host-tested)
    │   └── ridge_curve_generated.h  Cerro Autana's ridge, a height per column of
    │                           the boot photograph's frame      (host-tested)
    ├── input/          the devices a finger reaches
    │   ├── input.h             input_t, one frame's touch and buttons
    │   ├── touch.{h,c}         FT5x06 polling task
    │   ├── touch_fsm.{h,c}     samples -> press/release    (host-tested)
    │   ├── gesture.{h,c}       swipe recognition           (host-tested)
    │   ├── buttons.{h,c}       the power button
    │   ├── button_fsm.{h,c}    presses -> short/long       (host-tested)
    │   ├── imu.{h,c}, imu_rotation.h  the 6-axis IMU
    │   └── tilt.{h,c}          IMU counts -> down, strength, shake (host-tested)
    ├── util/           arithmetic and services that belong to no layer
    │   ├── fixed.h             fixed-point multiply/divide (host-tested)
    │   ├── intmath.h, rng.h, tween.h                       (host-tested)
    │   ├── trig.h              integer sine and cosine    (host-tested)
    │   ├── tune.{h,c}          numbers changed live over the console, dev builds (host-tested)
    │   ├── spring_line.h       a row of points on springs (host-tested)
    │   ├── job.{h,c}           run a slice on the other core (host-tested)
    │   ├── frame_cost.{h,c}    where a frame's time goes, by name (host-tested)
    │   ├── screenshot.h        BMP header + base64, pure   (host-tested)
    │   └── build_id.h          which build this is         (host-tested)
    ├── console/        the console listener, dev builds only - one verb per file
    │   ├── console_verbs.{h,c}    the registry and line dispatch (host-tested)
    │   ├── console_latch.h        a verb's request for the frame loop (host-tested)
    │   ├── console.{h,c}          driver install, the one reader task
    │   ├── console_buildid.c      buildid
    │   ├── console_tune.c         SET, GET, RESET, TUNE - forwards to util/tune
    │   ├── console_freeze.{h,c}   freeze, resume, step - holds the frame loop
    │   ├── console_screenshot.{h,c}  screenshot
    │   ├── device_state.{h,c}  a snapshot of the board's state, as JSON (host-tested)
    │   ├── console_runsuite.{h,c}    runsuite, CONFIG_LAUNCHER_SELFTEST only
    │   └── console_inject.c, console_inject_parse.h  TOUCH/IMU/TAP/PRESS/DRAG/BUTTON (host-tested)
    └── apps/           one folder per app - see Building-an-App.md
        ├── render_lab/ a software rasterizer, wireframe and ray-traced scenes
        ├── diagnostics/  bench tool; development builds only
        └── sand/       the falling-sand sandbox
```

**Includes are layer-qualified** - `"gfx/gfx.h"`, not `"gfx.h"` - including
between two files in the same folder. Uniform is the point: a reader should
not have to know where a file lives to read its includes, and an app reaching
past `ui` into `gfx` should be visible at the line that does it.

**Three words that are not interchangeable**, because the code uses all three
and means something different by each:

| | |
|---|---|
| **shell** | the frame loop and the app switching — `main.c`, whose log tag is literally `shell` |
| **launcher** | the home screen the shell draws when no app is running — `launcher/main/ui/ui_launcher.c`. This is what you reach after booting. |
| **boot** | what runs once before the loop exists and never again — `boot/` |

---

## Generated sources

Generated files live throughout the tree, each following the same rules
below — grep for `GENERATED FILE` to list them, since apps add their own. The
shell's own are `main/ui/control_center_layout_generated.h` (`tools/gen/gen_ui_layout.py`,
from `main/ui/control_center_layout.json`, which the host editor in
[`editor/`](../editor/README.md) edits), `main/ui/ridge_curve_generated.h`
(`tools/gen/gen_ridge_curve.py`, from `design/boot/ridge.png`, the ridge of
`design/boot/boot.png` drawn as a line in the same frame), `main/boot/boot_anim_curve.h` (`tools/gen/gen_zeta_curve.py`),
`main/boot/boot_anim_timeline.h` (`tools/gen/gen_boot_anim_timeline.py`, from
`main/boot/boot_anim_timeline.json`), `main/boot/boot_anim_image.h`
(`tools/gen/gen_boot_anim_image.py`, from `design/boot/boot.png`),
`main/gfx/gfx_palette_standard_generated.h` and `main/gfx/icons_system.h`.

`boot_anim_curve.h` holds the zeta function evaluated along the critical
line. That is not something to compute on this chip at the precision it
needs - the hardware FPU is single-precision only, and zeta along the
critical line needs double - and it never changes, so `tools/gen/gen_zeta_curve.py`
computes it once in double precision on a host and the result ships in
flash. `boot_anim_image.h` holds the same idea applied to a photograph the
boot animation crossfades to: there is no PNG decoder in this codebase, so
the pixel data - already rotated into panel space and packed into the
panel's own byte-swapped RGB565 - ships in flash the same way.

Five rules, and the fourth is the one that matters:

**The generator lives in `tools/`, the output in the tree it belongs to.**
Generated output is checked in, not built. A build-time generator would put
Python on the critical path of every clean build, on a project whose whole
toolchain story is already long enough.

**The output says so, and says how.** A banner naming the exact command that
produces it, on the first line, where someone about to hand-edit it will see
it before they start.

**The generator validates itself before emitting anything.** `gen_zeta_curve.py`
checks its own zeta against known values and exits rather than printing a
plausible-looking table of wrong numbers. A generator that half-works is worse
than one that fails, because its output looks fine.

**The shipped artifact is tested independently of the generator.** This is the
rule with teeth. A generator checking itself proves nothing about the file
actually in the repo, which can be stale, hand-edited, or produced by an older
version of the constants. So `suite_boot_anim.c` tests the numbers that ship,
against the mathematics rather than against the generator: the curve must
reach the axis at each of the five known zero heights, and must stay well
clear of it everywhere else. No table of plausible numbers passes both halves
by accident. `boot_anim_image.h` has no underlying math to check pixel
content against - its independent check is instead two `_Static_assert`s in
`boot_anim.c` pinning the shipped array's shape to the panel's own
`GFX_WIDTH`/`GFX_HEIGHT`, plus real visual verification through
`tools/boot_anim/boot_anim_editor_server.py`'s render view (the general path for a
render-affecting change is a `*_render_host.sh` harness diffed against its
`*_render_baseline.txt` with `tools/render/render_diff.sh` — see
[Render-Harness.md](tools/Render-Harness.md)).

**Two different rules for keeping a generated file current, by design.**
`boot_anim_editor_server.py`'s dev server updates both `boot_anim_timeline.h`
and `boot_anim_image.h` automatically, but not the same way. The timeline is
live state typed into the browser: `render()` writes it to a disposable
scratch copy on every scrub of the playhead, and only "Build & Flash"
overwrites the real, committed `boot_anim_timeline.json`/`.h` - a draft the
user is still editing must never land in the tree as a side effect of moving
a slider. The photograph has no draft concept - `design/boot/boot.png` is a
real file on disk, not something the editor holds live state for - so
`_ensure_image_current()` regenerates the REAL, committed `boot_anim_image.h`
in place whenever the PNG's mtime is newer, on every request, exactly like
running `gen_boot_anim_image.py` by hand would. The next asset type decides
which rule it follows the same way: state the browser is actively editing
gets a scratch copy and waits for an explicit save; a file on disk that the
generator only mirrors gets regenerated in place on demand.

---

## How it fits together

One row per dependency depth - the same thing the "includes are
layer-qualified" rule above makes visible at the line level, drawn
whole. Hardware-touching folders are marked.

```mermaid
flowchart TB
    classDef hw fill:#8a3d3d,color:#fff
    classDef contract fill:#f4f1e8,stroke:#333,stroke-width:1px,color:#111

    Apps["apps/<br/><i>one folder per app</i>"]
    Main["main.c<br/><i>the frame loop</i>"]
    Contract(["app.h - the shell/app contract"]):::contract
    Boot["boot/<br/><i>runs once, before the loop exists</i>"]

    subgraph T4[" "]
        Ui["ui/<br/><i>microui integration</i>"]
        Console["console/<br/><i>dev builds only</i>"]
    end
    subgraph T5[" "]
        Gfx["gfx/<br/><i>the one framebuffer</i>"]
        Render["render/<br/><i>3D transform, clip, projection</i>"]
        Display["display/<br/><i>orientation, with hysteresis</i>"]
        Input["input/<br/><i>touch, gesture, tilt</i>"]
    end
    subgraph T6[" "]
        Util["util/<br/><i>arithmetic and services</i>"]
    end
    subgraph T7[" "]
        Board["board/<br/><i>this board's pins and peripherals</i>"]
    end

    Apps ~~~ Main ~~~ Boot
    Boot ~~~ Ui
    Boot ~~~ Console
    Ui ~~~ Gfx
    Ui ~~~ Render
    Ui ~~~ Input
    Ui ~~~ Display
    Console ~~~ Gfx
    Console ~~~ Render
    Console ~~~ Input
    Console ~~~ Display
    Gfx ~~~ Util
    Render ~~~ Util
    Input ~~~ Util
    Display ~~~ Util
    Util ~~~ Board

    class Boot,Gfx,Input,Console,Board,Util hw

    %% The contract edge is index 17; edges above it shift that index.
    Contract -.->|"includes input/input.h"| Input

    linkStyle 17 stroke:#e11,stroke-width:2px
```

**A folder may include anything below it, and `app.h`, never above or
sideways within the same row.** `board/` sits in its own row below
`util/` - nothing in it includes another first-party folder, so it is
the tree's lowest layer. The red arrow marks the exception: `app.h`
includes `input/input.h`.

- **Every drawing path ends in gfx.** Nothing else allocates pixels. See
  [Gfx-and-Presentation.md](Gfx-and-Presentation.md#the-path) for how a draw
  call becomes pixels on the panel.
- **Hardware sits in the marked folders.** Within `input/`, the drivers
  (`touch.c`, `buttons.c`, `imu.c`) are split from the logic they feed,
  which is why `touch_fsm`, `button_fsm`, `gesture` and `tilt` are tested
  on a laptop.

---

## Three rules that shape everything

### 1. There is exactly one framebuffer

368 × 448 × 2 bytes = **322 KiB**, allocated in PSRAM
(`BOARD_FRAMEBUFFER_CAPS` in `board.h`), so it does not count against the
internal heap (see [Board-and-Memory.md](notes/Board-and-Memory.md)). There is
room in PSRAM for a second one; there is no time for it - a per-frame
catch-up copy between two PSRAM buffers costs more than sand's own frame
budget, and one full frame over QSPI is bandwidth-bound, not CPU-bound (see
[Display-and-Rendering.md](notes/Display-and-Rendering.md), "The blit is
bus-bound"), so a second buffer buys nothing on the send side either. The
decision and its measurements are in
[Autana-Rendering-Roadmap.md](Autana-Rendering-Roadmap.md) (decision B).

"One framebuffer" is really "one destination at a time": an app may ask for
a band ring or an index image at `enter()`, and gfx frees the framebuffer
while it holds one. The targets, the dirty tracker and the present path are
in [Gfx-and-Presentation.md](Gfx-and-Presentation.md).

This is also why the 3D renderer is small3dlib: it owns no framebuffer - it
hands back every rasterized pixel through a callback - and with
`S3L_Z_BUFFER 0` no depth buffer either, resolving visibility by sorting
triangles back-to-front. A conventional colour+depth rasterizer would want
~1.3 MB here.

### 2. There is exactly one frame loop, and it belongs to the shell

Apps do not loop, do not present, do not block and do not yield. An app's
`frame()` draws and returns. Consequences:

- switching apps is instant — no teardown of a running loop
- no app can wedge the device by forgetting to `vTaskDelay`
- the shell decides when to present, and can draw its own chrome afterwards

The one thing that runs a loop of its own is the startup animation, and it
runs *before* this one exists - see `boot_anim.c`. The rule is about apps: an
app must not loop because the shell has to stay able to switch away from it,
and at boot there is nothing to switch to yet. `show_post_failures()` blocks
for the same reason.

### 3. Apps are callbacks, not processes

One binary, one address space. There is no isolation: a misbehaving app can
corrupt the shell, and nothing stops a task on either of the chip's two cores
from touching another app's state. That is the accepted trade for instant
switching and no flash-partition machinery. Worth revisiting only if
third-party apps ever become a goal.

---

## Startup

Before the loop below ever runs, `app_main()` goes through a fixed order, and
the order is most of the point:

```
post_run_before_display()   the SD card, on its own independent SDMMC bus
gfx_init()                  panel up, framebuffer allocated
post_run_after_display()    the rest of the health check
                            -> a failure holds the screen for 8 s
selftest_run()              diagnostics builds only
ui_launcher_init()          the launcher exists, turned the way boot draws
boot_anim_run()             the startup animation, 5.5 s
touch_start(), buttons_start()
console_start()             the serial verbs
imu_init()
```

The animation goes after the health checks, so a board with a fault says so
before the device does anything decorative, and before touch starts, because
there is nothing yet for a tap to reach.

It ends by dissolving into the launcher, not by fading to black. For its
last 700 ms each frame starts from the home screen, painted by the shell
through `boot_anim_set_ending_backdrop()`, and the photograph and title
dither away over it. The launcher's ridge lies on the photograph's own, so
the mountain drops away and leaves its outline, and the last boot frame is
the launcher's first. `boot/` takes a painter because it knows nothing above
itself.

It draws three axes - the complex plane zeta's *value* lives in, as a floor,
and the height *t* up the critical line, straight up - and then plots
`zeta(1/2 + it)` climbing that axis. Where the curve touches the vertical
axis, zeta is zero, and those five points are the first five nontrivial zeros.

The camera orbits as the curve climbs, pitching from a side-on view toward a
near-top-down one in step with the curve's own progress, so the helix seen
from the side ends up reading almost face-on, as the spiral it always was -
see `boot_anim_view()` in `boot_anim.h`.

Three files, split by what can be tested where:

| | |
|---|---|
| `boot_anim_curve.h` | the curve, as a generated table - see [Generated sources](#generated-sources) for why it is baked on a host. |
| `boot_anim.h` | the spline, the colour and the timeline - integer arithmetic, no hardware header, so `test/suites/suite_boot_anim.c` checks all of it on a host. |
| `render/r3d_project.h` | the general camera-space near-plane clip and perspective projection, shared with a caller drawing something other than this timeline - `test/suites/suite_r3d_project.c` checks it on a host. |
| `render/r3d_camera.h` | the camera description `boot_anim_view()` builds and the viewport fit it reads centre/scale from, shared with a caller building its own camera - `test/suites/suite_r3d_camera.c` checks it on a host. |
| `boot_anim.c` | gfx calls and the loop. |

The suite checks the shipped table against the mathematics rather than against
the generator - the fourth rule under [Generated
sources](#generated-sources).

## The frame loop

The shell is a two-state machine. `current == NULL` means a system screen is
showing; anything else is the running app. Which system screen - the launcher
or Control Center - is `system_navigation_t`'s one field.

```mermaid
stateDiagram-v2
    direction LR
    [*] --> Launcher

    Running : Running<br/>one pass per frame
    Launcher : Launcher<br/>ui_launcher_frame()<br/>draws the app list
    ControlCenter : Control Center<br/>ui_control_center_frame()<br/>over the dimmed launcher

    Launcher --> Running: tap an entry<br/><i>the app's enter()</i>
    Running --> Launcher: home swipe, PWR long-press<br/>or shell_request_exit()<br/><i>the app's exit()</i>
    Launcher --> ControlCenter: swipe in from<br/>the logical top
    ControlCenter --> Launcher: swipe in from<br/>the logical bottom
```

What the shell does on each transition, and which way home an app gets, is
in [Building-an-App.md](Building-an-App.md#lifecycle). One pass of
the loop, with and without `update()`, is in
[Building-an-App.md](Building-an-App.md#one-pass-of-the-frame-loop).

### Apps with `update()`: overlapping the next step with the present

An app that sets `app_t.update` has the previous frame sent on core 1 while
`update()` runs on core 0. The split present underneath it is in
[Gfx-and-Presentation.md](Gfx-and-Presentation.md#present-who-runs-it).

### Full redraw

`gfx_request_full_redraw()` marks everything dirty and latches a pending
flag - see [Gfx-and-Presentation.md](Gfx-and-Presentation.md#repaint-controls).
gfx has no idea an app keeps its own draw cache beyond the framebuffer.
`main.c`'s `apply_pending_full_redraw()` checks the pending flag at the top
of a pass, clears it, and calls the running app's `invalidate()` - or
`ui_invalidate()` while the launcher is showing. Clearing before the draw
rather than after is what lets a request made inside that very `frame()` call
reach the following pass. The app's side is in
[Building-an-App.md](Building-an-App.md#full-redraw).

---

## Apps

How to write one - the `app_t` endpoints, registration, lifecycle and the
folder convention - is [Building-an-App.md](Building-an-App.md). What stays
here is why the build is shaped the way it is.

> **`WHOLE_ARCHIVE` is load-bearing.** The component becomes `libmain.a`, and a
> linker only extracts an archive member that resolves an undefined symbol.
> Since nothing references an app by name any more — the entire point — the
> object would never be extracted, its constructor would never run, and the app
> would silently vanish from the menu. Not a link error: a smaller binary and a
> shorter list.

**Bench-only apps** live in `apps/diagnostics/`, excluded by folder when
`CONFIG_LAUNCHER_DEVELOPMENT` is off — structural rather than a name check.
Diagnostics re-runs POST, which cycles the audio rail and re-mounts the SD
card, so it has no business being reachable in a shipped image. See
[Build-Variants](Build-Variants.md#release-builds-contain-no-test-code) — note in
particular that `REQUIRES` must **not** be gated this way.

Diagnostics ships in any development build, `--dev` included, not just
`--diag`. Its own toggle page mixes two shapes; the app itself does not.
The "run self test
suite" button and its result line are genuinely
SELFTEST-only (`#if CONFIG_LAUNCHER_SELFTEST` inside `app_diagnostics.c` —
`selftest_run()` does not exist as a symbol outside a SELFTEST build) and
compile out of `--dev`, while the POST report and the rest of the toggle
page (gfx debug overlays, interlace, the orientation readout) are
DEVELOPMENT-shaped and ship in both. Splitting that surviving DEVELOPMENT
content into its own Settings app is still open; see
[Settings-App-Plan.md](plans/Settings-App-Plan.md).

---

## Drawing a UI, in the shell or in an app

`launcher/main/ui/ui.c` owns the microui integration; `ui_launcher.c` is just one caller.
An app builds a UI the same way:

```c
mu_Context *ctx = ui_context();

ui_begin(input);
if (mu_begin_window_ex(ctx, "Settings", rect, opts)) {
    if (mu_button(ctx, "Clear")) { ... }
    mu_end_window(ctx);
}
ui_end(UI_NO_BACKGROUND);   /* draw over the app instead of clearing */
```

That gets the touch handling - which is not obvious, see the comment on
`feed_input()` - and the repaint logic below, for free.

### Styling a control

`ui_style.h` decides how a control's frame *looks*, separately from what it
*is*. Two styles exist for buttons:

| | |
|---|---|
| `UI_BUTTON_FLAT` | microui's own — a flat fill plus a one-pixel border. The default. |
| `UI_BUTTON_BEZEL` | lit from the top left, inverted while a finger is on it. What the launcher uses. |

```c
ui_begin(input);
ui_set_button_style(UI_BUTTON_BEZEL);   /* every frame - see below */
```

Three things about it are worth knowing before adding a style of your own.

**The hook is microui's, not a patch.** `mu_Context` carries a `draw_frame`
function pointer that every frame goes through — button, checkbox, slider,
scrollbar, window background — with a rect and a colour id. `ui_init()` saves
the one microui installed and puts its own in front, so `UI_BUTTON_FLAT` and
every non-button frame are still literally upstream's code, and nothing in
`components/microui/` is edited.

**A style emits commands, not pixels.** It would be simpler to call
`gfx_fill_rect()` and paint the edges directly, and it would break the repaint
skip below: that works by hashing microui's command list, so an edge drawn
outside the list is invisible to the hash and survives on screen as a stale
smear after the control underneath it changes. Styles return spans;
`styled_draw_frame()` turns spans into `mu_draw_rect()` calls; the hash sees
all of it.

**Style does not persist across frames.** `ui_begin()` resets it, so a caller
that wants a style states it every frame. That is the immediate-mode reading —
style is part of the frame's description, like everything else — and it is load
bearing here, because the whole shell shares one `mu_Context`: without the
reset, the launcher opting in would leave a running app's own buttons
bezelled too.

One detail worth spelling out, because it is the opposite of what a desktop
toolkit would do: **the pressed look is on hover, not only on focus.** On a
mouse, hover means "the pointer is near" and focus means "the button is
held"; on a touchscreen the pointer does not exist until a finger is already
on the glass, so hover *is* contact.

The pointer holds `DOWN` for the whole press instead of releasing the same
frame it presses — `ui_pointer.c` is where that policy lives — so
`MU_COLOR_BUTTONFOCUS` now covers most of a tap on its own —
microui keeps a control focused for as long as `mouse_down` stays true,
`MU_OPT_HOLDFOCUS` or not. What still needs hover is the one synthesized
frame *before* `DOWN` lands (see `feed_input()`'s comment): the pointer is
on the control but focus has not been taken yet, so a style keyed only on
focus would render that one frame flat. Keying the bezel off hover as well
as focus is what keeps it sinking smoothly through the whole gesture instead
of flashing in on the second frame.

The geometry and the shading are pure functions in the header, the same split
`icon_walk_blocks()` makes, so `test/suites/suite_ui_style.c` checks the shape on a host
without linking `gfx.c` or even `microui.c` — nobody can eyeball five
overlapping rectangles reliably.

`ui_style.h` has a flat sibling to the bezel above: `ui_panel_spans()` is a
face plus a plain border, for a captioned section frame that groups controls
without inviting a press — a panel outlines a whole screen area, a bezel
outlines one tap target, and the two would fight if a panel were lit and
shadowed the same way.

### Text at more than one size

A font and its scale ride inside every text command, so the repaint hash
sees a size change unaided; a text style does not. The calls and the reason
are in [Text-and-Fonts.md](Text-and-Fonts.md#text-in-a-microui-screen).

### App-owned artwork, and how it reaches the command list

`icon_walk_blocks()` (`gfx/icon.h`) fits an icon's row bytes to a destination
box and emits each horizontal run to a callback, so the per-draw stack is
O(1) in the icon's size; a per-icon bound is the baked `blocks` field of
`icon_t`. It stays pure geometry, the same split `ui_style.h`'s spans use: it
says WHERE the blocks go, not how they reach a framebuffer, so it links and
is tested on a host with no `gfx.c` or `microui.c` involved.

`ui_draw_icon(ctx, r, icon, rows, color)` (`ui/ui.h`) is what turns those
runs into command-list entries — one `mu_draw_rect()` per run. That is
the whole reason it exists, rather than an app calling `gfx_fill_rect()`
straight into the framebuffer for its own icon: artwork painted outside the
command list is invisible to the repaint hash and survives as a stale smear
once the control underneath it changes — the same "a style emits commands,
not pixels" rule above, applied to an app's own artwork instead of a
control's frame. It also means an app icon needs no new `MU_ICON_*` id and
no patch to `components/microui/`.

The icons themselves are never the shell's to own. The system atlas stays
what the shell itself needs: `MU_ICON_CHECK` maps to `ICON_SYSTEM_CHECK` in
`gfx/icons_system.h`, and the other three microui icons stay a centred-square
placeholder — see the comment on `MU_COMMAND_ICON` in `ui.c` for why that is
deliberate. An app that wants a funnel, a cross, a
starburst draws its own bitmaps in its own folder (e.g.
`apps/sand/icons_sand.h`) and reaches `ui_draw_icon()` to put them in its
own command list. Deleting the app folder deletes its icons with it, per
"an app is a folder" above.

`ui_slider_int()` (`ui/ui_build.c`) is built the same way, one layer down:
`ui/ui_slider.h` is pure geometry — the track, the filled portion and the
knob rect for a value, and the inverse, a touch x back to a value,
quantized and clamped — and `ui_slider_int()` turns that into
`mu_draw_rect()` calls via `ui_panel_spans()`/`ui_bezel_spans()`. Integer
throughout, deliberately: these are whole-unit settings, and
`mu_slider_ex()`'s float value and `"%.2f"` thumb are the wrong shape for
that. A slider is also the one control that actually needs the pointer to
hold `DOWN` for the whole press rather than release on the same frame it
pressed — see "the pressed look is on hover" above for that policy and why
it lives in `ui_pointer.c`; without it, a drag can only jump to where a
finger first landed and then goes deaf to everything after.

### Immediate mode versus dirty bands

These fight, and the fight would have hit apps, not just the launcher.
Immediate mode rebuilds and repaints the UI every frame, which means clearing
every frame, which marks every band dirty and forces a full transfer
(`gfx.h`'s QSPI note) - discarding the saving that partial updates exist to
provide.

The resolution: an immediate-mode UI is **rebuilt** every frame but not
necessarily **changed**. microui's command list is a complete description of
the output, so two frames that hash the same *are* the same picture.

A retained-mode engine knows what changed because changing it is an explicit
act - you mutate a node, it marks its canvas dirty. Immediate mode throws that
signal away by construction, so we recover it from the other end: compare
output where an engine compares intent. Same destination, opposite direction.

### One window is one canvas, in full-framebuffer mode

The canvas split comes free with it. microui already groups commands by root
container, each with its own rect, so each window is hashed, repainted and
band-marked on its own. A live readout in one window does not force a static
toolbar in another to repaint - which is the point of splitting canvases at
all.

```mermaid
flowchart TB
    BUILD["ui_end()"] --> HASH["hash each window's<br/>command range"]
    HASH --> CMP{"differs from<br/>last paint?"}
    CMP -->|no| DIRTY{"its bands already<br/>dirty underneath?"}
    CMP -->|yes| PAINT["repaint this canvas"]
    DIRTY -->|no| SKIP(("skip<br/><i>no draw, no transfer</i>"))
    DIRTY -->|yes| PAINT
    PAINT --> OVER["also repaint any<br/>window above it<br/>that overlaps"]
```

Band mode has no framebuffer to hash canvases against, so it hashes shapes
by row range instead, per band (`ui_band_hash`, `ui/ui.c`).

Two rules that have to be respected:

- **Painter's order.** Windows are drawn back to front, so repainting one means
  repainting anything above it that overlaps, or the repaint erases what was on
  top.
- **`ui_invalidate()`.** If something replaced the screen in a way `ui_end()`
  cannot detect - returning to the launcher after an app has been running - the
  UI must be told its pixels are gone. Otherwise it compares an unchanged
  command list, skips, and leaves the app's last frame on screen.

A screen whose command list does not change paints and sends nothing at all,
and reaches the tick ceiling - the launcher does so with
`ui_ridge_set_ambient()` off. `test/suites/suite_ui.c` covers
the independence claim directly - it builds two windows, changes one, and
asserts the other's bands stay clean.

### Dimming what is behind a panel (the scrim)

A panel over a paused app reads as pasted on unless whatever is behind it is
knocked back. The pattern, used by two of the sand app's screens:

1. the panel keeps `UI_NO_BACKGROUND`, so the frozen app stays visible in
   the gaps rather than being cleared away;
2. once, when the panel opens, dim the whole canvas —
   `gfx_fill_rect_blend(0, 0, GFX_WIDTH, GFX_HEIGHT, black, alpha)`;
3. draw the panel over it, opaque.

No new primitive: `gfx_fill_rect_blend()` already mixes into the destination.

**The rule that makes it work: apply it exactly once per repaint of what is
underneath, never per frame.** A blend fill *reads* the pixel it writes, and
the app behind a panel is frozen — nothing repaints it while the panel is up
— so a second application lands on the first one's own output. Repeat it per
frame and the backdrop walks toward black while the user sits there.
`suite_gfx_color.c` pins the arithmetic so the rule cannot quietly rot into a
comment nobody believes. Cost says the same thing independently: this reads
every pixel on the panel, which `gfx.h` warns is not what a blend fill is
for. Once is free; every frame is neither correct nor affordable.

In practice the backdrop is genuinely fresh at exactly two moments — the
frame the panel opens, and a turn taken while it is open (which repaints the
app underneath). See `dim_backdrop()` in `apps/sand/app_sand.c`.

**The general form, when once is not enough.** "Once" is a global sequencing
rule, and those rot. The local version: *whoever repaints a region restores
the app underneath it first, then re-scrims that region, then draws.* The
app's own partial-repaint machinery is what makes this affordable, so the
cost is one panel's worth of repaint, not a whole canvas, and only while
someone is actually interacting.

That form is strictly more robust and is the **precondition for genuinely
translucent panels**: a panel you can see through has to be composited over
fresh pixels every repaint, so it cannot use the once-only shortcut at all.
The simple version above is the special case that suffices while panels are
opaque and do not move — nothing ever reveals backdrop that was not scrimmed,
and nothing repaints backdrop that was. Switch to the general form when
either of those stops being true.

**Why this one is allowed to paint pixels**, when `ui_style.h` insists a
style must emit commands: everything in the command list is re-emitted on
every repaint, and re-emitting is precisely what a scrim must never do. It
is not part of the picture the hash describes; it is a one-off change to
what the picture is drawn *on top of*. A scrim expressed as a command would
be a scrim applied every repaint, which is the bug above.

---

## Text and fonts

What a font is, the role accessor, how text is drawn and how to add a
typeface: [Text-and-Fonts.md](Text-and-Fonts.md).

---

## Input

`touch.c` polls the FT5x06 at 100 Hz on its own task, above the render loop's
priority. Sampling is decoupled from rendering on purpose: a frame is ~24 ms
and a quick tap can be shorter, so polling once per frame drops taps.

`touch_fsm` interprets the samples and latches press/release edges, so an event
that happens entirely between two frames still reaches the next one. Edges are
consumed when read, so each is delivered exactly once.

Two hardware facts drive the design, both documented in the platform notes:
reads are gated on the INT line because the controller NACKs when idle, and a
release needs a 60 ms quiet period because INT means "data ready" rather than
"finger down" and drops briefly mid-touch.

---

## Why microui, not LVGL

The Waveshare BSP lives in `components/esp32_s3_touch_amoled_1_8/` with its
LVGL interface removed. LVGL is not built. `gfx.c` drives the panel directly
through the panel drivers; the BSP supplies board services and touch setup.

Three constraints, all already documented elsewhere in this project, point
the same direction once put next to each other:

**The internal-heap budget is tight even with the framebuffer moved to
PSRAM.** The framebuffer does not compete for
internal SRAM - it lives entirely in the board's 8 MB of octal PSRAM (see
`docs/notes/Board-and-Memory.md`) - but internal (non-PSRAM) free heap is
what `DP_FREE_HEAP_BYTES` records, nowhere near gigabytes, and a persistent
widget tree and
style system are a standing tax on that pool for the life of the process,
not a one-time cost. microui needed patching too - see "The vendored header
is patched" below - but that is a one-time struct-layout edit down to a small
fixed size, not a standing tax the way a persistent widget tree and style
system are.

**This device runs apps that own their entire framebuffer.** The falling-sand
simulation and Render Lab each drive the panel directly, on their own
schedule, with no widget tree in between. A retained-mode toolkit wants to
own the display and the refresh cycle - exactly what those apps already do
for themselves. microui's command-list model asks for nothing: it turns a UI
description into a list of rectangles, text and icons once a frame, and
whoever is drawing paints that list whenever they like, into whatever they
like. It composes with an app that owns its own frame loop; a retained-mode
toolkit would compete with it for the same job.

**The whole render pipeline here is built around skipping unchanged frames**,
because a full transfer is bus-bound and costs most of a frame - see
[Display-and-Rendering.md](notes/Display-and-Rendering.md), "The blit is
bus-bound" - and most frames do not need one. An
immediate-mode command list is exactly the shape that trick needs - see
[Immediate mode versus dirty bands](#immediate-mode-versus-dirty-bands)
above for the mechanism. LVGL has its own separate invalidation and redraw
system, built around owning the display - adopting it would mean reconciling
two damage-tracking systems, or replacing the one already built for every
other app, rather than reusing it for free.

**The real cost, for balance:** microui encodes a mouse's interaction model
(point, then click), and a touchscreen cannot produce that sequence - the
pointer does not exist until a finger is already down. Every control needs
synthesised hover frames to compensate - `UI_POINTER_HOVER_FRAMES`
(`ui/ui_pointer.h`) of input latency on every tap. That friction is specific to picking an immediate-mode,
mouse-shaped toolkit; a touch-native widget system would not have it. It was
worth paying given the three constraints above, but it is a real trade-off,
not a free win.

## The microui integration

microui is immediate-mode and draws nothing itself: each frame it turns the UI
description into a list of rectangles, text and icons, and `ui.c` walks
that list painting into the framebuffer.

That command-list model is why it suits this device - see [Why microui, not
LVGL](#why-microui-not-lvgl) above for the argument.

### Two things to know before touching it

**The vendored header is patched.** Upstream sizes `mu_Context` for desktop —
the command list alone is 256 KiB, which does not fit beside the framebuffer.
Sizes are reduced in `components/microui/include/microui.h`, with upstream
values in trailing comments. They are edited in the header rather than
overridden from our side because they determine the struct's layout, and two
translation units disagreeing would corrupt it silently.

**Touch needs two synthesised hover frames.** `mu_update_control()` only
establishes hover on a frame where the button is *not* held, and a control only
submits once focused — the mouse sequence "point, then click". A touchscreen
never produces the first half, because the pointer does not exist until a finger
is already down.

Two frames, not one, and the second is the one that is easy to miss:
`mu_mouse_over()` needs `in_hover_root()`, and `mu_begin()` copies `hover_root`
from the *previous* frame's `next_hover_root`. So the first frame at a new
position only tells microui which window the finger is in; the second is the
first that can mark a control hovered; the press follows. `ui_pointer.c` owns
that policy (`UI_POINTER_HOVER_FRAMES`) and `suite_ui_pointer_microui.c` pins
it against real microui.

A press shipped one frame early holds `mouse_down` from the press frame
onward, so hover is never established, nothing takes focus, and **every
button in the shell draws its pressed frame while returning 0** — no app
reachable from the launcher. Event-list unit tests cannot catch this, since
hover establishment is microui's own state, which is why
`suite_ui_pointer_microui.c` drives real microui.

**This applies to every microui control**, not just buttons — anything reacting
to a press goes through `mu_update_control()`. Adding a checkbox requires
nothing new, but reworking input handling means preserving this.

A slider needed one more thing the hover synthesis alone does not give: the
hold-`DOWN` policy `ui/ui_pointer.c` owns - see "the pressed look is on
hover" under "Drawing a UI" above for what it does and why - and
`ui_slider_int()` is the control that could not exist without it.

---

## Related

- `docs/notes/` — the hardware constraints underneath all of this: memory
  budget, panel gotchas, touch quirks, flashing and recovery. Start at
  `docs/notes/README.md`.
- `docs/sand/Sand-Simulation.md` — the falling-sand app in depth: materials, the
  water model, momentum, and why its liquid logic is its own file.
- `docs/Testing-Guide.md` — how to test any of it.
- `docs/Build-Variants.md` — what release, dev and diagnostics builds each
  carry, and which flag gates what.
- `docs/plans/Settings-App-Plan.md` — planned split of the Diagnostics app's
  developer-toggle page into its own Settings app.
