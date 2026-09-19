# Building a Screen

Start here to build or change a UI screen in this
shell. Read this start to finish before writing anything. For how the UI
works see [`Launcher-Architecture.md`](Launcher-Architecture.md). This page
is instructions, not narrative.

## The loop

0. **Decide what owns what, before any code.** Three files, three jobs, and
   the split is what makes a screen testable at all:
   - **layout** (`<screen>.c/.h`) - pure geometry, canvas width and height
     taken as parameters. No `gfx.h`, no hardware.
   - **state** (`*_ui.c/.h`) - which screen is up, what a click MEANS, what
     the screen remembers. Pure, no drawing.
   - **drawing** (`apps/<app>/ui/<screen>.c/.h`) - the microui calls that
     build this screen's command list: `ui_begin_screen()` through
     `mu_end_window()`, taking a `mu_Context*` and a small state struct
     rather than reaching for `app_*.c`'s own statics. `app_*.c` keeps
     input, gfx calls, timing and app-state ownership, brackets the call
     with `ui_begin()`/`ui_end()`, and applies whatever the screen reports
     was clicked. This is what makes a screen's OWN drawing host-testable -
     see `ui/suite_command_list_budget.c` beside each app's screens for
     one that drives the real function against a real microui and asserts
     its command-list use fits `MU_COMMANDLIST_SIZE`.

1. **Write the layout module first, and test it before drawing anything.**
   Every rect the screen needs, from one function. It is the cheapest thing
   to get right and the most expensive to retrofit.

2. **Put the screen's fixed strings in the layout module**, beside the rects
   they must fit, and measure them in the test. A caption the layout has
   never seen is a caption nothing can prove fits.

3. **State machine next**, still without drawing. The caller hit-tests and
   reports WHICH thing was hit; this module decides what that means.

4. **Then draw.** By this point the geometry and the behaviour are both
   already pinned by host tests, and the drawing code is mostly transcription.

5. **Verify in order:** `run_tests.sh`, then `check_app_sources.sh` (the only
   thing that compiles `app_*.c` without a device), then flash and look.

Never invert 1 and 4. A layout bug found on glass costs a flash cycle; the
same bug costs a second on a laptop.

## Exact commands

```sh
./launcher/test/run_tests.sh          # host suites, <1 s - the TDD loop
./launcher/test/check_app_sources.sh  # compiles app_*.c against host stubs
./launcher/tools/build_flash.sh --dev # --dev, always: screenshot.sh needs it
./launcher/tools/screenshot.sh -o shot.png   # lossless PNG, does not reset
```

A screen can also be rendered on a host, with no board and no flash cycle,
through the real drawing code and the real `gfx.c`:

```sh
./launcher/tools/render_all_scenes.sh        # every declared scene
./launcher/tools/post_ui_render_host.sh      # one of them
./launcher/tools/render_diff.sh shot.png /tmp/post/landscape-panel.bmp
```

A screen built through microui is rendered the same way, driven over
several frames with a declared synthetic touch. Declaring a scene for your
own screen is two files, and diffing one against a device capture is one
command - both in
[`Testing-Guide.md`](Testing-Guide.md)'s host render harness section. Every
scene stands in the data its screen normally gets - a fixture table, a
timestamp - so nothing a render shows came off hardware.

`idf.py -B build.dev build` is also worth running for anything touching
device-only files: it is a real cross-compile and catches what host stubs
cannot.

## House rules

These are not style preferences. Each one is a bug that shipped.

- **Never hand-roll a hit test.** Go through a real control, or build one
  from `mu_get_id()` + `mu_update_control()`. Hand-tested coordinates do not
  survive a transform.
- **Never assume `GFX_WIDTH`/`GFX_HEIGHT` in layout.** Ask `ui_width()` /
  `ui_height()`; they swap under a quarter turn.
- **Never paint pixels outside the command list.** `ui_end()` hashes that
  list to skip repaints, so anything drawn behind its back survives as a
  stale smear. The scrim is the one deliberate exception, and only because
  it must *not* be re-applied per repaint.
- **Every tap target is at least 44px** in its smaller dimension.
- **Assert every layout invariant at both 368x448 and 448x368.**
- **Styles are part of the frame's description.** `ui_begin()` resets the
  button style; state what you want every frame.

### Anchoring a fixed rectangle

`ui_anchor_rect()` puts an element pivot on an anchor in its parent. The
named `UI_ANCHOR_*` points select the parent corner, edge centre or centre;
the pivot selects the same normalized point within the element. Offsets are
pixels from that landing point. `ui_rect_inset()` makes a safe area by
insetting each parent edge. Resolve the rectangle in the upright logical
canvas, then map it with `ui_transform_rect()`.

## How to do the things a screen usually needs

### A control microui does not have

`mu_button()` centres one label and cannot stack an icon over text. Build it
the way `ui_slider_int()` does: `mu_get_id()` from a unique name,
`mu_update_control()`, then read
`ctx->mouse_pressed == MU_MOUSE_LEFT && ctx->focus == id` for the click and
emit the frame, artwork and label as commands. Ids must be unique per
control or two of them collide.

Hand the click to the state module; let it decide what it means.

### More than one text size

`ui_set_font_scaled(gfx_font_ui(), scale)`. The UI font is the 1bpp 8x8
bitmap, so integer scales stay crisp; an 8bpp atlas font would blur above 1.

**Do not add a render-time global for a UI setting.** Anything read at
render time is invisible to the repaint hash and needs `ui_invalidate()` on
every change - which a two-size screen hits every frame, defeating the skip
entirely. Settings that ride *inside* the command list (the font, and so the
scale) are free. Ask which kind you are adding before you add it.

### Text that must fit

Measure it: `ui_measure_text()`, or `gfx_font_text_width()` in a host test.
Fonts, scales and text styles are in [`Text-and-Fonts.md`](Text-and-Fonts.md).

### A list of rows that may overflow

`ui_scroll.h` (`launcher/main/ui/`) is the shared way to lay out a stack of
centred, fixed-width rows and let it scroll once it no longer fits - the
boot menu, the launcher list and a runtime-options menu all build on it
instead of each hand-tracking a `y` or placing rows at an ABSOLUTE rect,
which left every row past the first unreachable once the stack overflowed
(only a RELATIVE `mu_layout_set_next()` folds into a container's own
`content_size` and follows its scroll - see `microui.c`'s `mu_layout_next()`).

Open the window with `ui_scroll_view_begin()` instead of `ui_begin_screen()`,
close it with `ui_scroll_view_end()`, and lay out rows with an `ui_flow_t`
cursor:

```c
if (ui_scroll_view_begin(ctx, "My Screen", opt, ui_scroll_view_default(), dt_ms)) {
    ui_flow_t flow = ui_flow_start(ui_width(), top, gap);
    for (int i = 0; i < count; i++) {
        ui_flow_row(ctx, &flow, row_w, row_h);
        if (mu_button(ctx, labels[i])) { chosen = i; }
    }
    ui_scroll_view_end(ctx);
}
```

`ui_flow_top(canvas_h, count, row_h, gap, margin)` gives the starting `top`
for a uniform stack that should sit centred when short and pinned to
`margin` once it no longer fits - the same rule the boot menu already used
for its own row count.

`ui_scroll_view_config_t` (from `ui_scroll_view_default()`, or built by hand)
controls what a plain `ui_begin_screen()` cannot: `axis` (which of
`UI_SCROLL_AXIS_{NONE,VERTICAL,HORIZONTAL,BOTH}` a drag or scrollbar may
move - vertical only by default), `hide_scrollbar` (draw no scrollbar
chrome; dragging the content still scrolls it), and `momentum_tau_ms` (0 by
default - a drag stops dead on release; above 0, residual velocity decays
exponentially with that time constant, integrated in closed form from
`dt_ms` so a coast covers the same distance at any frame rate). `dt_ms` is
required even when momentum is off, since a screen that starts with it off
may not stay that way.

### Artwork

`ui_draw_bitmap()` emits a bitmap as run-length rects into the command list.
Application artwork lives in the app's own folder so deleting the app
deletes it. Structural facts only in tests - non-empty, bbox in range, run
count under the cap, declared symmetries - never assert artwork against the
code that draws it.

**Icons are always baked assets.** Do not assemble a symbol from
`mu_draw_rect()` or other drawing primitives inside a renderer. Shared UI
vocabulary belongs in `design/icons/system.*` and is baked into
`gfx/icons_system.h`; app-owned symbols use the same generator inside the
app's folder. Drawing primitives remain appropriate for geometry such as
panels, tracks and status indicators, not pictograms.

### A panel over a paused app

1. `ui_end(UI_NO_BACKGROUND)` so the frozen app survives in the gaps.
2. Dim it **once** with `gfx_fill_rect_blend()` - see the scrim section in
   [`Launcher-Architecture.md`](Launcher-Architecture.md), and note it is
   once per repaint of the backdrop, never per frame.
3. Draw the panel over it.

On close, force a full repaint of the app underneath and reset any
accumulators the pause built up.

### Knowing what a screen costs

`MU_COMMANDLIST_SIZE` is 8 KiB and everything drawn spends it - roughly 250
rects for a whole screen. A `CONFIG_LAUNCHER_DEVELOPMENT` build logs the
high-water mark from `ui_end()`. Check it before adding a texture or a
fifth icon.

## Testing rules specific to UI

- **Layout, state and geometry are all host-testable. Test them there.**
  Only drawing needs a device.
- **A pure module's unit test does not test its consumer.** If the consumer
  is portable, link it and drive it. `suite_ui_pointer_microui.c` exists
  because an event-list suite stayed green while no button in the shell
  could be pressed.
- **Watch every behavioural test fail first, with a mutation that COMPILES.**
  A stub that trips `-Werror` prints no test lines and proves nothing - grep
  the run for `error:` to be sure you saw an assertion.
- **Vary the fixture's arbitrary starting condition** and confirm the
  assertion still catches what it claims.
- **A UI suite's file-scope objects are firmware `.bss`.** Diagnostics
  builds link every suite, and UI fixtures are exactly the ones that get
  large - a microui context alone is 10,744 bytes, real weight against
  internal heap headroom. Allocate them in `fixture()`, and run
  `tools/build_diag_check.sh` to build that configuration locally rather
  than finding out from CI.

## Checklist before flashing

- [ ] layout asserted at both orientations, nothing overlapping or off-canvas
- [ ] every fixed string measured against its own rect
- [ ] every tap target >= 44px
- [ ] clicks routed through a real control, decided by the state module
- [ ] nothing painted outside the command list
- [ ] `run_tests.sh` green, `check_app_sources.sh` green
- [ ] new behavioural tests seen red first, on a mutation that compiled

## Related

- [`Building-an-App.md`](Building-an-App.md) - the app a screen lives in
- [`Text-and-Fonts.md`](Text-and-Fonts.md) - fonts, scales, text styles
- [`Launcher-Architecture.md`](Launcher-Architecture.md) - the mechanisms
- [`Testing-Guide.md`](Testing-Guide.md) - suites, runners, build variants
