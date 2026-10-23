# Building a Screen

Build or change a UI screen here. A screen has three separable parts:
rectangles and text that fit them, the state changed by input, and the drawing
calls. The host renderer can show the real drawing code without a board;
[the render harness](../tools/Render-Harness.md) has a sample image and command.
For the shell's frame ownership see [Firmware](../firmware/README.md).

To inspect an existing screen first, run
[`launcher_home_render_host.sh`](../../launcher/tools/render/scenes/launcher_home_render_host.sh) from Git Bash on
Windows or a terminal on Linux. Its `results/render/launcher_home/`
folder contains BMP output and PNG output if Pillow is installed. You need a
host C compiler, as described in the [README](../../README.md#try-it-without-a-board).

## The face

A screen includes `ui/ui.h`, plus the toolkit headers that do not reach the
backend (`ui/layout/ui_anchor.h`, `ui/controls/ui_icons.h`). Its verbs take
plain values: a `recti_t` for where, a string for what, a pointer for the
value a control edits, `0xRRGGBB` for a colour. The library behind them is
private to `ui/`, and the include gate refuses it anywhere else, so the
backend can change without touching a screen.

```c
static void
settings_draw(settings_state_t* s) {
    ui_screen();
    ui_row(UI_ROW_HEIGHT);
    ui_slider(ui_next(), "VOLUME", &s->volume, 0, 10);
    ui_row(UI_ROW_HEIGHT);
    ui_toggle(ui_next(), "MUTE", &s->muted);
    ui_row(UI_ROW_HEIGHT);
    ui_dropdown(ui_next(), "OUTPUT", &s->output, OUTPUTS, OUTPUT_COUNT);
    ui_row(UI_ROW_HEIGHT);
    if (ui_button(ui_next(), "RESET")) {
        s->reset_requested = true;
    }
}

void
settings_frame(const input_t* input, settings_state_t* s) {
    ui_begin(input);
    settings_draw(s);
    ui_end(0x0A0C14);
}
```

| A screen needs | Verbs |
|---|---|
| a frame | `ui_begin()`, then `ui_end()` or `ui_end_over()` |
| a window | `ui_screen()`, `ui_window(r)`, `ui_scroll_window(r)` |
| rows that flow down the window | `ui_row(h)` or `ui_row_centre(w, h)`, then `ui_next()` for the rect |
| controls | [`ui_button`](button.md), [`ui_button_icon`](button_icon.md), [`ui_tile`](tile.md), [`ui_tile_icon`](tile_icon.md), [`ui_toggle`](toggle.md), [`ui_slider`](slider.md), [`ui_dropdown`](dropdown.md), [`ui_dropdown_icons`](dropdown_icons.md) |
| text | [`ui_text`, `ui_text_right`, `ui_text_centre`, `ui_caption`, `ui_caption_centre`](text.md), [`ui_header`](header.md) |
| marks | [`ui_panel`](panel.md), `ui_fill`, `ui_outline`, `ui_bevel`, [`ui_icon`, `ui_check`](icon.md), [`ui_swatches`](swatches.md) |
| a look for the screen | `ui_theme(&theme)` once per screen; `NULL` restores the shell look |
| a look for a few widgets | [scopes](scopes.md): `ui_color_begin/end`, `ui_scale_begin/end`, `ui_alpha_begin/end`, `ui_disabled_begin/end` |

- **Rect first.** Every widget takes its rect. A screen with fixed geometry
  passes rects from its layout module; a flowed screen passes `ui_next()`.
- **The caller owns every value.** A toggle, slider or dropdown writes
  through its pointer and returns true on the frame it changed. A button or
  tile returns true on the frame a tap is released on it.
- **A label is the identity.** See [identity](input.md#identity).
- **Scopes balance.** See [scopes](scopes.md).

The verbs and their arguments are in [`ui/ui.h`](../../launcher/main/ui/ui.h);
each control's page, with its example, renders and states, is listed in
[`ui/README.md`](README.md#controls).

## The loop

0. **Decide what owns what, before any code.** Three files, three jobs, and
   the split is what makes a screen testable at all:

   - **layout** (`<screen>_layout.c/.h`): pure geometry, canvas width and
     height taken as parameters, rects as `recti_t`. No `gfx.h`, no
     hardware.
   - **state** (`*_ui.c/.h`): which screen is up, what a tap MEANS, what
     the screen remembers. Pure, no drawing.
   - **drawing** (`apps/<app>/ui/<screen>.c/.h`): the face calls that build
     this screen, taking a small state struct rather than reaching for the
     app's own statics. A host suite in the app's `tests/` drives the real
     function and reads back what it built through `launcher/test/ui_probe.h`.

1. **Write the layout module first, and test it before drawing anything.**
   Every rect the screen needs, from one function. It is the cheapest thing
   to get right and the most expensive to retrofit.

2. **Put the screen's fixed strings in the layout module**, beside the rects
   they must fit, and measure them in the test. A caption the layout has
   never seen is a caption nothing can prove fits.

3. **State machine next**, still without drawing. The drawing reports WHICH
   control was hit; this module decides what that means.

4. **Then draw.** By this point the geometry and the behaviour are both
   already pinned by host tests, and the drawing code is mostly transcription.

5. **Verify in order:** `run_tests.sh`, then `check_app_sources.sh` (the only
   thing that compiles `app_*.c` without a device), then flash and look.

Never invert 1 and 4. A layout bug found on glass costs a flash cycle; the
same bug costs a second on a laptop.

## Exact commands

```sh
./launcher/test/run_tests.sh          # host suites: the TDD loop, see testing/README.md
./launcher/test/check_app_sources.sh  # compiles app_*.c against host stubs
autana flash dev                      # --dev, always: autana screenshot needs it
autana screenshot --framebuffer -o shot.png  # lossless PNG, does not reset
```

A screen can also be rendered on a host, with no board and no flash cycle,
through the real drawing code and the real `draw.c`:

```sh
./launcher/tools/render/render_all_scenes.sh        # every declared scene
./launcher/tools/render/scenes/post_ui_render_host.sh -o /tmp/post  # one of them
./launcher/tools/render/render_diff.sh shot.png /tmp/post/landscape-panel.bmp
```

A scene drives its screen over several frames with a declared synthetic
touch. Declaring a scene for your own screen is two files, and diffing one
against a device capture is one command; both are in
[`tools/Render-Harness.md`](../tools/Render-Harness.md). Every scene stands in
the data its screen normally gets: a fixture table, a timestamp, so nothing
a render shows came off hardware.

`idf.py -B build.dev build` is also worth running for anything touching
device-only files: it is a real cross-compile and catches what host stubs
cannot.

## House rules

These constraints keep input, layout, and rendering behavior consistent.

- **Never hand-roll a hit test.** A tap is a control's: `ui_button()`,
  `ui_tile()` and the rest. Hand-tested coordinates do not survive a
  transform.
- **Never assume `GFX_WIDTH`/`GFX_HEIGHT` in layout.** Ask `ui_width()` /
  `ui_height()`; they swap under a quarter turn.
- **Never paint pixels outside the face.** `ui_end()` hashes what the frame
  built to skip repaints, so anything drawn behind its back survives as a
  stale smear. The scrim is the one deliberate exception, and only because
  it must *not* be re-applied per repaint.
- **Every tap target is at least `UI_TAP_MIN`** in its smaller dimension, and
  a new control should aim for `UI_TAP_RECOMMENDED`; `ui/ui.h` gives both and
  why.
- **Assert every layout invariant at both 368x448 and 448x368.**
- **Styles are part of the frame's description.** `ui_begin()` resets the
  button style; state what you want every frame.

### Anchoring a fixed rectangle

`ui_anchor_rect()` (`ui/layout/ui_anchor.h`) puts an element's pivot on an
anchor in its parent. The named `UI_ANCHOR_*` points select the parent
corner, edge centre or centre; the pivot selects the same normalized point
within the element. Offsets are pixels from that landing point.
`ui_rect_inset()` makes a safe area by insetting each parent edge.
`DISPLAY_PANEL_SAFE_INSET` and `DISPLAY_PANEL_CORNER_RADIUS` are the two
glass facts every layout clears. Resolve the rectangle in the upright
logical canvas, then map it with `ui_transform_rect()`.

## How to do the things a screen usually needs

### A control the face does not have

Add it to `ui/` as a face verb in `ui/ui.h`, not as a private copy in one
screen. It takes a rect and plain values, draws in the theme's colours,
and gets a host test, an example and a page;
[`ui/README.md`](README.md#adding-to-the-toolkit) lists what a new
control must have. Hand its result to the state module;
let that decide what it means.

### More than one text size

`ui_scale_begin(scale)` / `ui_scale_end()` around the text, or a theme's
`text_scale` for the whole screen. The UI font is the 1bpp 8x8 bitmap, so
integer scales stay crisp.

**Do not add a render-time global for a UI setting.** Anything read at
render time is invisible to the repaint hash and needs `ui_invalidate()` on
every change; a two-size screen hits every frame, defeating the skip
entirely. Settings that ride *inside* what the frame builds (the font, and
so the scale) are free. Ask which kind you are adding before you add it.

### Text that must fit

Measure it: `ui_measure_text()`, or `draw_font_text_width()` in a host test.
Face text is one clipped line per rect. Fonts, scales and text styles are in
[`Text-and-Fonts.md`](../gfx/Text-and-Fonts.md).

### A list of rows that may overflow

Open the window with `ui_scroll_window(r)` and flow the rows with `ui_row()`
and `ui_next()`; [scrolling](scrolling.md) has the example and when a
moving finger is a drag.

### Artwork

`ui_icon(r, icon, fallback)` draws a one-bit `draw_image_t` filling `r` in
the text colour. Application artwork lives in the app's own folder so
deleting the app deletes it. Structural facts only in tests: non-empty,
bbox in range, run count under the cap, declared symmetries; never assert
artwork against the code that draws it.

**Icons are always baked assets.** Do not assemble a symbol from `ui_fill()`
or other marks. Shared UI vocabulary belongs in the engine pack's system set
(`launcher/main/engine/icons/`); app-owned symbols are an icon set in the
app's own pack ([icons](../tools/Icon-Baker.md)), loaded with
`ui_icon_set_load()` (`ui/controls/ui_icons.h`). An icon can be missing when
its pack is, so a control keeps a label or a short text to show instead.
Marks remain appropriate for geometry such as panels, tracks and status
indicators, not pictograms.

### A panel over a paused app

1. `ui_end(UI_NO_BACKGROUND)` so the frozen app survives in the gaps.
2. Dim it **once** with `draw_rect_blend(0, 0, GFX_WIDTH, GFX_HEIGHT,
   black, alpha)`, once per repaint of the backdrop, never per frame: the
   blend reads the pixel it writes, so a second pass darkens the first.
   Repaint the backdrop and dim again on a full-redraw request or a turn.
   Why this is a pixel write and not a face call is
   [the scrim](../firmware/UI.md#the-scrim-the-one-thing-drawn-outside-the-command-list).
3. Draw the panel over it.

On close, force a full repaint of the app underneath and reset any
accumulators the pause built up.

### A screen over a backdrop that is drawn

`ui_end_over(paint_backdrop)` is `ui_end()` for a backdrop that is a picture
and not a colour; the launcher's ridge (`shell/ui/ui_ridge.c`) is the model.

1. `paint_backdrop` paints the whole screen. It is called only when the UI
   itself changed, and every window is then painted over it.
2. A backdrop that animates draws its own changed part **before**
   `ui_end_over()`. That dirties the screen under an unchanged UI, which is
   put back on top with no backdrop call.
3. At rest it must draw nothing. A backdrop that redraws an identical frame
   dirties the screen, and the launcher stops being free while idle.
4. React to the raw `input_t`, not to the controls: a press is held back until it is known to be a tap and not a
   scroll, which is a frame or two too late for feedback.

### Knowing what a screen costs

Everything a frame builds spends the device's fixed command list, roughly
250 rects for a whole screen. A `CONFIG_LAUNCHER_DEVELOPMENT` build
logs the high-water mark from `ui_end()`, and a screen's budget suite calls
`ui_probe_budget_assert()` to keep headroom for one more screen. Check it
before adding a texture or a fifth icon.

## Testing rules specific to UI

- **Layout, state and geometry are all host-testable. Test them there.**
  Only drawing needs a device.
- **Read what the frame built through `ui_probe.h`.** It answers where a
  widget landed (`ui_probe_widget()`), where and in what colour text was
  drawn, and whether a dropdown's list is open, in plain values, so a suite
  includes nothing of the backend either.
- **A pure module's unit test does not test its consumer.** If the consumer
  is portable, link it and drive it with real taps through `ui_begin()`.
  An event-list suite once stayed green while no button in the shell could
  be pressed.
- **Watch every behavioural test fail first, with a mutation that COMPILES.**
  A stub that trips `-Werror` prints no test lines and proves nothing; grep
  the run for `error:` to be sure you saw an assertion.
- **Vary the fixture's arbitrary starting condition** and confirm the
  assertion still catches what it claims.
- **A UI suite's file-scope objects are firmware `.bss`.** A diagnostics
  build links every suite, and UI fixtures are exactly the ones that get
  large: real weight against internal heap headroom. Allocate them in
  `fixture()`, and run `tools/build/build_diag_check.sh` to build that
  configuration locally rather than finding out from CI.

## Checklist before flashing

- [ ] layout asserted at both orientations, nothing overlapping or off-canvas
- [ ] every fixed string measured against its own rect
- [ ] every tap target >= `UI_TAP_MIN`
- [ ] taps routed through a face control, decided by the state module
- [ ] no toolkit header that reaches the backend included
- [ ] nothing painted outside the face
- [ ] `run_tests.sh` green, `check_app_sources.sh` green
- [ ] new behavioural tests seen red first, on a mutation that compiled

## Related

- [`Building-an-App.md`](../firmware/Building-an-App.md): the app a screen lives in
- [`ui/README.md`](README.md): the toolkit behind the face, pictured
- [`Text-and-Fonts.md`](../gfx/Text-and-Fonts.md): fonts, scales, text styles
- [`firmware/README.md`](../firmware/README.md): the mechanisms
- [`testing/README.md`](../testing/README.md): suites and runners
- [`Build-Variants.md`](../tools/Build-Variants.md): what release, dev and
  diagnostics builds carry
- [`tools/Render-Harness.md`](../tools/Render-Harness.md): rendering a screen
  on a host, and diffing it against a capture
