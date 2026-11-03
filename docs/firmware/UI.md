# UI

`ui/ui.c` owns the microui integration; the launcher is a caller like any
app. The controls are catalogued in [UI-Toolkit.md](../ui/README.md), and a
screen is put together as in [Building-a-Screen.md](../ui/Building-a-Screen.md).
This page is how the integration underneath works.

## Immediate mode versus dirty bands

Immediate mode and dirty bands fight. Immediate mode clears and repaints every frame, which marks every
band dirty and forces a full transfer, discarding what partial updates save.

An immediate-mode UI is **rebuilt** every frame but not necessarily **changed**.
microui's command list describes the output completely, so two frames that hash
the same are the same picture.

A retained-mode engine knows what changed because changing is an explicit act.
Here the signal is recovered from the other end: compare output where an engine
compares intent.

## One window is one canvas

microui groups commands by root container, each with its own rect. Each
window is hashed, repainted and band-marked on its own, so a live readout
does not repaint a static toolbar in another window.

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

Band mode has no framebuffer to hash against, so it hashes shapes by row
range, per band (`ui_band_hash`, `ui/ui.c`).

| Rule | Why |
|---|---|
| Painter's order: repainting a window repaints any window above it that overlaps | windows draw back to front, or the repaint erases what was on top |
| `ui_invalidate()` after anything that replaces the screen unseen, such as returning to the launcher from an app | otherwise the UI compares an unchanged command list, skips, and leaves the app's last frame on screen |

A screen whose command list does not change paints and sends nothing.
`test/suites/suite_ui.c` changes one of two windows and asserts the other's
bands stay clean.

## The scrim: the one thing drawn outside the command list

A panel over a paused screen dims what is behind it with one full-screen
`draw_rect_blend()`. It is a pixel write, not a command, because a blend
reads the pixel it writes: the command list is re-emitted on every repaint,
so a scrim in it would walk the frozen backdrop toward black.

- It is applied **once per repaint of what is underneath**: when the panel
  opens, and whenever the backdrop is redrawn (a full-redraw request, a turn).
- That holds while panels are opaque and still. A translucent or moving panel
  needs the general form: whoever repaints a region restores the backdrop under
  it, re-scrims that region, then draws.
- [suite_image_pixel.c](../../launcher/packages/image/tests/suite_image_pixel.c) pins the arithmetic; the app recipe is
  [a panel over a paused app](../ui/Building-a-Screen.md#a-panel-over-a-paused-app).

## Why microui, not LVGL

The Waveshare BSP lives in `components/esp32_s3_touch_amoled_1_8/` with its
LVGL interface removed; LVGL is not built. `board/waveshare_amoled_1_8/board_panel.c` brings the
panel up, `gfx/present/gfx_present.c` sends to it, and `board/` wraps the BSP for
board services and touch setup.

| Constraint | Why it points to microui |
|---|---|
| Internal heap is tight, even with the framebuffer in PSRAM | a widget tree and style system are a standing tax on internal SRAM; microui is one fixed-size context, cut down once in its header ([patched header](#the-vendored-header-is-patched)) |
| Apps own their whole frame | an app draws straight to the framebuffer on its own schedule; a retained toolkit wants the display and the refresh cycle, a command list asks for nothing |
| The pipeline skips unchanged frames, because a full transfer is bus-bound | a command list is the shape that needs ([dirty bands](#immediate-mode-versus-dirty-bands)); LVGL would add a second damage tracker to reconcile |

**The cost:** microui encodes a mouse (point, then click) and a touchscreen
cannot produce the "point". The shell looks the press up in the last frame and
patches microui to take hover on the press itself
([press on contact](#a-press-lands-on-the-frame-of-contact)), so a tap still
answers on its first frame.

## Two things to know before touching it

### The vendored header is patched

Upstream sizes `mu_Context` for a desktop, with a 256 KiB command list alone.

The sizes are reduced in `components/microui/include/microui.h`, upstream values in
trailing comments, rather than overridden from our side: they fix the struct's
layout, and two translation units disagreeing would corrupt it silently.

### A press lands on the frame of contact

microui hovers a control only on a frame where the button is *not* held, and
submits only once it is focused. A local change lets the first control under a
press take hover on the frame the button goes down, so overlapping controls
never share one press. First means first built, the one drawn underneath: a
control acts while it is built, before any later one exists, so where controls
overlap in one window the one on top cannot take the press on contact. Put a
control meant to sit on top in its own window instead. Hover also needs `hover_root`, which `mu_begin()` copies from the
*previous* frame, before the finger existed.

| Step | Owner | Does |
|---|---|---|
| look up | `ui/input/ui_bridge.c` | on the press frame, before the pointer steps: `mu_seed_hover_root()` picks the root from the last frame's `root_list`, `mu_scrollable_at()` asks whether the point is on content in that root that scrolls |
| press | `ui/input/ui_pointer.c` | sends MOVE and DOWN at once; on scrollable content only the MOVE, the press waiting for the lift or a sideways move |
| confirm | `ui_bridge.c` | after the frame, records `press_missed` (the seed was not the root the frame found, and nothing took focus) and asks `mu_scrollable_at()` again |
| again | `ui_pointer.c` | a missed press is made again on the next frame until it lands; on content that scrolls it is taken back and waits |

```mermaid
sequenceDiagram
    participant P as ui_pointer
    participant B as ui_bridge
    participant M as microui
    Note over P,M: frame of contact
    B->>M: seed hover root, look for scrollable content
    P->>B: MOVE and DOWN
    Note over M: hover, focus and submit
    B->>P: press_missed = stale seed and no focus
    Note over P,M: next frame, only if missed
    P->>B: MOVE and DOWN again
    Note over M: hover, focus and submit
```

- A screen switch, a list closing or a window's first build leaves the seed
  stale; the press is made again on the next frame and the tap is never lost,
  except a press and release inside that one frame.
- A stale seed whose root was still built presses what was on screen there; it
  counts as landed, so a tap never clicks twice.
- A repaint-only frame (`ui_begin(NULL)`, the boot backdrop) feeds no input and
  does not step the pointer. It runs before `input_start()`, with no touch.
- `ui_pointer.c` also holds the `DOWN` a slider needs to track a drag, and the
  UP owed when a lift never reached the UI (`ui/input/ui_pointer.h`).
- A press or lift within `snap.reach` of a control is aimed at its nearest edge;
  the raw press stays the drag origin and later motion stays raw.

The rule applies to every control that reacts to a press, so reworking input
handling must preserve it. `suite_ui_pointer_microui.c` and
`suite_ui_widgets.c` pin it against real microui: hover is microui's own state,
which event-list tests cannot see.
