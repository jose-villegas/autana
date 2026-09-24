# Plan: a UI editor, and the layout format underneath it

**Status**: phases 1 to 4 exist for one screen. The editor is
[`editor/`](../../editor/README.md); Control Center is its one authored
document and the launcher a preview beside it. The brush screen is not yet a
document.

The objective is a tool where a screen is **authored visually and edited
again later** - not screenshotted and re-typed. The brush screen should open
in it as an editable instance. It is a component of the engine direction in
[`../Autana-Rendering-Roadmap.md`](../Autana-Rendering-Roadmap.md), sibling
to the level editor already banked in that roadmap.

---

## What exists

| piece | where |
|---|---|
| authored layout | `launcher/main/ui/<screen>_layout.json`: the screen's name, its elements (`id`, `label`, `interactive`) and one rect per element per orientation |
| generator | `launcher/tools/gen_ui_layout.py`, one for every screen; every C identifier derives from the JSON's `screen` |
| baked table | `<screen>_layout_generated.h`, the only form the device links |
| document | `editor/src/layout_document.{h,cpp}`, one type for every screen, applying the generator's rules |
| renderer | `editor/runtime/runtime.c`: the firmware's `ui/` and `gfx.c` compiled for the host |
| shell | `editor/src/main.cpp`: SDL2 + Dear ImGui; hierarchy, both orientations side by side, inspector, undo, explicit save and bake |

A screen becomes editable with three things: its JSON, a draw function taking
the baked struct (`ui_control_center_draw()` is the model), and an entry in
`runtime.c` and in `load_system_screens()`. Nothing else in the editor names a
screen.

The format holds **rects and nothing else**. Of the six layout concepts and
the text constraint described below, none is expressed yet; every rect is
absolute per orientation. That was enough for Control Center and will not be
for a flowed list such as the launcher, which is why the launcher is only
previewed.

## This pattern already runs in this repo

`tools/boot_anim_editor_server.py` serves `tools/boot_anim_editor.html` and
answers `POST /render` with **a real frame rendered by the real firmware
code** - `main/gfx/gfx.c` plus `main/boot/boot_anim.c`, unmodified, compiled
for the host. It writes the edited payload to a *scratch* copy of
`boot_anim_timeline.json` placed on the include path ahead of the committed
one, so a draft never touches the real header. It hashes the payload so
scrubbing time never recompiles, and it has a build-and-flash path out the
back.

That is the architecture, running, for one payload. A level editor
proposes it for a second. This plan is the third:

| | authored data | generator | rendered by |
|---|---|---|---|
| boot animation | `boot_anim_timeline.json` | `gen_boot_anim_timeline.py` | real `boot_anim.c` + `gfx.c` on host |
| level editor | material blocks | bake to a header | real sand code on host |
| **UI editor (this)** | **a screen's layout** | **bake to a header** | **real `gfx.c` + pure geometry on host** |

They are not three tools. They are one pattern with three payloads, and the
pattern is already proven.

## The one thing that has to change

`brush_screen_layout()` computes rects from arithmetic over `#define`s -
`HEADER_H`, `MODE_H`, `SIZE_H`, gaps, `UI_MARGIN`, a centred remainder. **An
editor cannot edit arithmetic, only data.**

So a screen becomes authored JSON, baked by a generator into a header that
the same pure function consumes. Exactly the move the boot animation already
made, and the fifth instance of the generated-file convention in docs/Launcher-Architecture.md
(banner naming the regenerate command, generator validates before emitting,
shipped artifact tested independently of the generator).

**The device never sees the editor, and never sees JSON.** It links a static
baked table: no runtime layout engine, no solver, no allocation, no RAM
cost - the same deal fonts, icons and the boot timeline already have. That
distinction is the whole reason this is affordable here and LVGL was not:
the cost of a retained UI system is paid at build time, on a laptop, or it
is not paid at all.

## What already exists, and does not need building

- **The render path.** `apps/sand/tools/brush_screen_preview.c` renders the
  screen at both orientations, on a host, through the real `gfx.c` and the
  real `ui_style.h` / `ui_slider.h` / `gfx/icon.h` geometry. That is what a
  `/render` endpoint needs; it is already written.
- **Host-linkable everything.** `gfx.c` (behind its `ESP_PLATFORM` guards),
  the pure geometry headers, the baked icon atlases, and `microui.c` - the
  last of these linked for `suite_ui_pointer_microui.c` and available now.
- **The validation.** `suite_brush_screen.c` already asserts, at both
  368x448 and 448x368: everything inside the canvas, no panel overlap, equal
  segment widths, a `UI_TAP_MIN` floor on every tap target, and every fixed string
  measured against its own rect.

That last one matters more than it looks - see below.

## The data model: grow it from need, and do not build a solver

The trap is a general constraint system. That is the LVGL mistake relocated
to build time: it makes the model unbounded and turns the editor into a
programming language with a mouse.

What the screens in this tree actually need today is roughly six concepts: a
vertical stack, fixed-versus-fill heights, gaps, margins, centred slack, and
rows within a panel. Express those, and add a seventh when a real screen
demands it rather than in anticipation.

**Text is a first-class constrained thing, not a string dropped in a rect.**
Both real defects the brush screen shipped were text-versus-box - a value
box sized by eye at 64px for a string needing 80, and a caption row 232px
wide for wording needing 240 - and the weakness the host preview then found
in portrait is a *scale policy* failing: a long material name steps down
until it is the same size as the caption above it, and the type hierarchy
collapses. So an entry carries its string source, its scale policy and its
box **together**. An editor that cannot say "this will not fit at this
scale" would let you draw those same bugs, visually, and call it a design.

## The editor is a native app, and renders in-process

Two decisions, and the first one forces the second.

**Native, cross-platform, not a web page.** One source tree runs on
Windows, Linux and macOS, with nothing Windows-specific. Web stays a
*target* - something the engine may one day be built for - never the way
the editor draws itself.

**It links the firmware's host-portable C in-process**, rather than spawning
a renderer and reading back an image. `boot_anim_editor_server.py` spawns
and recompiles because its payload is baked into a header the C reads; there
is no way around it there. Layout-as-data removes that tax entirely: nothing
is generated to preview a change, so the editor can mutate a struct, call
the same layout and draw code the firmware calls, and re-render at frame
rate. Direct manipulation needs that - dragging a panel through a subprocess
round trip per frame is not the same product.

That choice also serves the web goal instead of fighting it: the same
host-portable C compiles under Emscripten, so a browser preview later is the
same code, not a second implementation of it.

### The shell: SDL2 + Dear ImGui

Cross-platform makes the *dependency* the deciding question, not the widget
set.

**SDL2 + Dear ImGui** is the shell. SDL supplies the window, input and the
texture the framebuffer is blitted into; Dear ImGui supplies docking, the
hierarchy, the inspector, menus and text editing. The price is a C++
toolchain, confined to `editor/`: it links the firmware's C through one C
header (`editor/include/editor/runtime.h`) and never enters an ESP-IDF
component graph. All four dependencies are fetched at configure time at a
pinned tag, never vendored.

**raylib + raygui** is the pure-C alternative. It makes a first preview
cheaper, and then the hierarchy, inspector, docking and text input become
editor chrome to write by hand.

**SDL2 + microui** would build the editor with the toolkit it edits - every
gap in `ui/` found by daily use. It loses on the same count, harder: microui
has no real text input, no file dialogs and no docking.

**Build with CMake.** ESP-IDF already uses it, so it is not a new tool for
anyone on any platform. The editor is never part of the firmware build, and
is absent from `idf.py` and from `test/run_tests.sh`; its own suite is CTest,
run by `.github/workflows/host-tests.yml`.

## Phases

1. **Layout as authored data.** A screen's JSON, a generator, a baked header,
   and the screen's draw function reading the table. *Exists for Control
   Center.*

   For a screen that already computes its rects, the acceptance is that its
   suite passes untouched and **the baked rects are identical to what the
   function produced**. Same discipline the icon baker used - a generator
   that cannot reproduce known-good output is not ready to produce new
   output. `brush_screen_layout()` is the first such screen.

2. **Validation in the generator.** It refuses, at bake time and for every
   orientation, a layout that overlaps, leaves the canvas or drops an
   interactive element below `UI_TAP_MIN`. *Exists*, mirrored in the document so the
   editor reports the same problems while dragging. **Not yet: a string in a
   box it does not fit** - the defect this plan cares most about. The host
   suite keeps its own assertions as the independent witness - the generator
   checking itself is not a test.

3. **The editor shell.** A native window that links the layout and draw code
   directly and renders both orientations side by side. No server, no
   subprocess, no recompile in the preview loop. *Exists.*

4. **Direct manipulation.** Drag and resize on the canvas, numeric edits in
   the inspector, undo and redo, writing back to the JSON. *Exists.* The
   document writes the file itself, one rect per line, so an edit's diff is
   the rects that moved: a format only a GUI can produce is a format nobody
   can review.

5. **A second screen proves the model.** The brush screen, then the palette.
   A format that has only ever expressed one screen has proven nothing about
   being a format, and these two need the stack, fill and text concepts the
   format still lacks.

6. **Cost overlays.** The panel's bands and dirty cells drawn over the canvas
   in the user's orientation, optional snapping to them, and a per-element
   cost hint - the same reactive text costs a hundred times more across
   bands than along one. Guidance in the tool, never a rule in the format.

## Considered and rejected

- **A runtime layout engine.** The thing microui was chosen over. Layout
  resolved on device costs RAM and cycles for a picture that is identical
  every frame; bake it instead.
- **A general constraint solver.** Unbounded model, unbounded editor, and
  every screen in this tree is a stack of panels. Revisit only when a real
  screen cannot be expressed.
- **Round-tripping generated C.** The generated header is output and never
  input - the convention every other generated file here already follows.
  Parsing back what a generator emitted is how the authored source and the
  artifact drift.
- **A JavaScript reimplementation of the renderer for the editor.** The boot
  animation editor rejected exactly this and says so in its own header: it
  renders through the real C rather than a JS twin. A preview that is not
  the shipping renderer is a preview of something that does not exist.

- **A browser-hosted editor at all**, which is what the boot animation
  editor is. It suits a timeline with a scrubber; a layout editor wants
  direct manipulation, and that wants in-process rendering. Accepted
  consequence: two editor architectures coexist until the older one is
  either migrated or retired. Not a reason to make this one a page.

- **Anything platform-specific** - Win32, WinUI, Cocoa, GTK-only. One source
  tree has to serve Windows, Linux and macOS, which is the same bar every
  shell script here already meets.
- **Editing the mockup instead.** A design image is an input to authoring,
  not the authored artifact. The brush screen already diverges from its
  mockup in two accepted places, and those decisions live in the plan, not
  in a PNG.

## Related

- [`../Building-a-Screen.md`](../Building-a-Screen.md) - how a screen is built by hand today
- [`../tools/Icon-Baker.md`](../tools/Icon-Baker.md) - the same authored-data-to-baked-header pattern, for artwork
- [`../Autana-Rendering-Roadmap.md`](../Autana-Rendering-Roadmap.md) - the engine direction this serves
