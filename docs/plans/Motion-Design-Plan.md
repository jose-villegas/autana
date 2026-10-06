# Plan: motion design for the launcher's interactions

**Status**: planned; nothing built. Costs marked *est* are arithmetic from the
docs cited, not board measurements.

The goal is motion that explains where things come from: the Control Center
slides down from the edge it is pulled from, an app grows out of its launcher
row, a blurred backdrop keeps the screen underneath legible, and a quarter turn
morphs one layout into the other instead of cutting.

## What the board allows

On this board, sending pixels costs more than drawing them, so motion must
change as little of the screen as possible per frame.

| Fact | Value | Source |
|---|---|---|
| Full-frame present | 9.6 ms at 80 MHz, 17.6 ms at 40 MHz | device present tests in `launcher/test/suites/suite_gfx.c` |
| Fixed cost per panel transaction | about 118 us | device tests in `launcher/test/suites/suite_gfx.c` |
| Small moving partial regions at 80 MHz | corrupt pixels; a full-frame send heals them | `docs/notes/Display-and-Rendering.md` |
| PSRAM read / PSRAM-to-PSRAM copy | 33-58 MB/s / about 22 MB/s | `docs/plans/Autana-Rendering-Roadmap.md` |
| Dirty cells | a 7 x 4 grid of boxes with no preferred axis: a send covers the changed boxes, in either orientation | `docs/Gfx-and-Presentation.md` |
| Launcher frame today | about 24.5 ms, presented synchronously (*est* 14 ms draw + 10 ms send) | measured with injected taps |
| Band ring | frame time becomes the larger of render and send | `docs/plans/Autana-Rendering-Roadmap.md` |

What already exists:

- `util/motion/tween.h`: progress is a byte (0-255). Over 448 px that is about
  1.75 px per step, so an eased end visibly stair-steps. Spatial motion
  needs Q16 progress.
- `util/motion/spring_line.h`: integer springs on a fixed 4 ms tick that come to
  rest by themselves.
- `ui/ui_transform.h`: axis-aligned translate and scale. Text turns only in
  quarter turns and scales only by whole numbers.
- `ui/control_center_layout_generated.h`: the portrait and landscape layouts
  share element ids, so the two layouts are already paired for a morph.
- `ui/ui_control_center.c`: the backdrop is the launcher under an alpha-230
  scrim (about 90% black). A blur behind it would not show.

## The pieces, in build order

| # | Piece | Approach | Cost (*est*) | Verdict |
|---|---|---|---|---|
| 1 | Motion module in `util/`, host-tested, time passed in | Critically damped half-life spring that keeps its velocity when the target changes, for anything the finger drives or can interrupt; 33-entry Q16 easing tables baked from Material 3's emphasized curves for fixed transitions; always sample by elapsed time | A few operations per animated value | Build first |
| 2 | Control Center slide | An opaque sheet follows the finger on the edge swipe, then springs open or closed from the release velocity; the launcher behind it freezes (ridge motion paused); one full-frame send at the end heals the 80 MHz partial-update corruption | Portrait about half a present per frame (5 ms); landscape about a full send | Build second, and measure both orientations: that number sets the budget for everything after |
| 3 | Cached blurred backdrop | On open: downsample the framebuffer 4x, three box-blur passes (within about 3% of a Gaussian), bake in the tint, keep it; upsample with ordered dither when drawn; the scrim gets much lighter than 230 | 20 KB PSRAM; one hitch of about 20 ms (8 ms downsample read, 1.5 ms blur, 8-12 ms upsample) that can be spread over the first slide frames; nothing per frame while static | Fit |
| 4 | App open | Container transform: grow the row's own card or colour to full screen, then cut to the app's first full redraw; also hides the app's start-up time | Fills only, dirty area = the card; 3-10 ms present | Fit |
| 5 | App close | The framebuffer holds the app's last frame at exit: copy it to a half-resolution thumbnail (82 KB PSRAM) and shrink it onto its row, drawn in band-ring mode to avoid bulk PSRAM writes | Scaling 3-5 cycles/px plus the PSRAM read (6-10 ms at full size, about 4x less at half) | Fit with caveat |
| 6 | Orientation morph | For each element id, lerp its old physical rect to the new one (each baked layout through its own quarter transform); stage it: bounds first, fills and colours lerped, text hidden and cut to the new orientation, faded in at the end; launcher rows pair by app | Fills per frame, full send per frame | Fit; the most work |

Ruled out:

| Idea | Why not |
|---|---|
| Drawing the app into a shrinking viewport | Apps draw at fixed full-screen coordinates, and text scales only by whole numbers |
| Rotating a snapshot of the old screen (the iOS turn) | *est* 30-50 ms per frame: a rotated walk over PSRAM misses a cache line nearly every pixel |
| Shifting framebuffer rows in place for slides (Pebble's compositor) | Rows are PSRAM-to-PSRAM here, about 15 ms per full frame; redrawing is cheaper |
| Kawase blur | Depends on free GPU bilinear taps; running box sums win on a CPU |

## Rules for every transition

- Pick the pattern from the spatial relationship: a container grows into a
  page, peers slide along one axis (Material's motion system).
- Enter with emphasized-decelerate `(0.05, 0.7, 0.1, 1)`, exit with
  emphasized-accelerate `(0.3, 0, 0.8, 0.15)`. Full-screen spatial springs
  use damping 0.9, stiffness 300; small ones damping 0.9, stiffness 1400.
- A dropped frame skips a position, never stretches the duration. Below
  about 25 fps, shorten the transition or cut across the middle with a few
  eased frames at each end, as Pebble's "moook" curve does, rather than
  running slower.
- An interrupted animation retargets from where it is, carrying its
  velocity.
- Animation state lives in a small internal-RAM table keyed by `mu_Id` and
  pruned by the last frame that touched it, the usual way an
  immediate-mode UI keeps per-control state.
- Honour a reduce-motion setting once a Settings screen exists.

## Open questions only the board answers

1. The real cost per frame of a translated Control Center sheet in portrait
   and landscape, from the frame-cost report (`docs/tools/Frame-Cost.md`).
2. PSRAM read throughput for thumbnail scaling and blur upsampling while
   core 1 is presenting: is it band-ring bound or PSRAM bound?
3. At what scrim alpha a 1/4-resolution RGB565 blur reads as blur, rather
   than mush or banding, on this AMOLED.

## Sources, read first

| Topic | Read |
|---|---|
| Principles | [Material 3 motion tokens](https://github.com/material-components/material-components-android/blob/master/docs/theming/Motion.md); [WWDC23 "Animate with springs"](https://wwdcnotes.com/documentation/wwdc23-10158-animate-with-springs/); Chang & Ungar, [*Animation: from cartoons to the user interface*](https://dl.acm.org/doi/10.1145/168642.168647), UIST 1993; Heer & Robertson, [*Animated transitions in statistical data graphics*](https://idl.cs.washington.edu/files/2007-AnimatedTransitions-InfoVis.pdf), InfoVis 2007 (staging) |
| Springs and easing | Daniel Holden, [*Spring-It-On*](https://theorangeduck.com/page/spring-roll-call); Ryan Juckett, [*Damped springs*](https://www.ryanjuckett.com/damped-springs/); Allen Chou, [*Precise control over numeric springing*](https://allenchou.net/2015/04/game-math-precise-control-over-numeric-springing/); PebbleOS [`animation_timing.c`](https://github.com/coredevices/PebbleOS/blob/main/src/fw/applib/ui/animation_timing.c) and [`animation_interpolate.c`](https://github.com/coredevices/PebbleOS/blob/main/src/fw/applib/ui/animation_interpolate.c) |
| Transitions on a slow link | PebbleOS [`compositor_slide_transitions.c`](https://github.com/coredevices/PebbleOS/blob/main/src/fw/services/compositor/default/compositor_slide_transitions.c) and the frame-interval note in [`animation.h`](https://github.com/coredevices/PebbleOS/blob/main/src/fw/applib/ui/animation.h); [Inside Playdate](https://sdk.play.date/inside-playdate) |
| Blur | Apple [UIImageEffects](https://developer.apple.com/library/archive/samplecode/UIImageEffects/Listings/UIImageEffects_UIImageEffects_m.html); Mario Klingemann, [Stack Blur](https://quasimondo.com/2004/02/25/stackblur-2004/); [Image-Kernels-Plan.md](Image-Kernels-Plan.md) |
| Layout morph, immediate mode | Paul Lewis, [FLIP](https://aerotwist.com/blog/flip-your-animations/); Compose [shared elements](https://developer.android.com/develop/ui/compose/animation/shared-elements); Flutter [Hero](https://api.flutter.dev/flutter/widgets/Hero-class.html); Ryan Fleury, [*UI Part 2*](https://www.dgtlgrove.com/p/ui-part-2-build-it-every-frame-immediate); [ImAnim](https://github.com/soufianekhiat/ImAnim) |
