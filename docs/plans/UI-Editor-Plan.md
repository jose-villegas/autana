# UI editor: authoring a project's screens

> The editor code is parked on the `feature/archive-editor` branch; it is not built or maintained on main.

The editor authors screens as data, previews through the project's draw code,
and bakes tables for the project's build. It opens a project folder rather
than naming firmware screens. The project supplies `<screen>_layout.json`
and one registered draw entry per screen.

**Status:** the document model, validation, editing controls and layout baker
exist in [`editor/`](../../editor/README.md). Project loading and registered
draw entries are planned; no project screen has an authored document.
The first document is the brush screen.

## Project boundary

| Owner | Responsibility |
|---|---|
| editor | open a project folder, list its registered screens, edit their documents, call their draw entries |
| project folder | `<screen>_layout.json`, screen draw entries and the project's assets |
| `ui/`, `draw/`, `gfx/` | public toolkit, drawing and presentation compiled for the host |
| project build | link `<screen>_layout_generated.h`; no JSON parser or editor dependency |
| `shell/ui/` | launcher and ridge as hand-written C using the public toolkit |

The editor links no shell or app source. A project draw entry accepts a layout
value so direct manipulation needs no compiler, subprocess or generated
header in the preview loop. The launcher has no editor preview until the
shell opens as a project. System gestures, app switching and boot handoff
remain privileged shell code outside the public toolkit.

## Existing owners

| Piece | Owner | Current surface |
|---|---|---|
| document and validation | `editor/src/layout_document.{h,cpp}` | screen name, elements and a rectangle per orientation |
| header emission | `bake_header()` in `editor/src/layout_document.cpp` | identifiers derived from the JSON's `screen` |
| command-line baker | `editor_layout_bake`, wrapped by `launcher/tools/gen/bake_ui_layout.py` | validates before emitting a C header |
| editor window | `editor/src/main.cpp` | hierarchy, orientation canvases, inspector, undo, explicit Save and Bake |
| geometry constraints | `editor/include/editor/runtime.h` | panel dimensions and `UI_TAP_MIN` |
| brush geometry | `apps/sand/ui/brush_screen.h` | `brush_screen_layout()` computes rectangles |
| brush host rendering | `apps/sand/tools/brush_screen_render_host.c` | real screen drawing in both orientations |
| independent brush checks | `suite_brush_screen.c` | bounds, overlap, segment widths, tap targets and text fit |
| framebuffer preview pattern | `launcher/tools/render/` | host-portable drawing code renders into a buffer |

Save and Bake are separate actions. The document writes one rectangle per
line, keeping diffs reviewable. The generated-file banner names the
regenerate command; the drift gate verifies the committed output.

## Layout data

| Concept | Current format | Planned addition when a screen needs it |
|---|---|---|
| screen and elements | `screen`, `id`, `label`, `interactive` | project registration binds the screen to its draw entry |
| orientations | absolute rectangles for portrait and landscape | the same document drives both previews |
| composition | rectangles only | vertical stacks, fixed or fill heights, gaps, margins, centred slack and rows |
| text | label string | string source, scale policy and box validated together |

Validation refuses overlap, out-of-bounds rectangles and interactive targets
smaller than `UI_TAP_MIN`. Text validation must also refuse a string that
does not fit its box at the chosen scale. A long label must retain the
screen's type hierarchy. Add layout concepts from concrete screen needs;
there is no general constraint solver.

The project build consumes static tables. Layout parsing, validation and
composition belong to authoring and baking, with no runtime solver or
allocation required on the target.

## Native editor

| Choice | Purpose |
|---|---|
| SDL2 | cross-platform window, input and framebuffer texture |
| Dear ImGui | docking, hierarchy, inspector, menus and text editing |
| C++ confined to `editor/` | desktop authoring; the toolkit keeps a C boundary |
| in-process C drawing | mutate a layout, call the project's draw entry, upload pixels |
| CMake and CTest | standalone build and tests, outside the project target's build |
| pinned fetched dependencies | build-directory dependencies without vendoring |

Windows and Linux use one source tree. A browser remains a possible target
for the same portable drawing code, rather than a separate renderer.

## Work to complete

| Work | Acceptance |
|---|---|
| project folder and draw registration | the editor names no target-specific screen; each project registers its own entries |
| first document: brush screen | baked rectangles equal `brush_screen_layout()`; the independent screen suite and pinned render stay unchanged |
| text fit validation | document and baker report the same string, scale and box errors; independent host assertions remain |
| live project preview | dragging mutates layout data and calls the real draw entry in both orientations |
| palette screen | a second screen exercises shared composition and text concepts |
| optional cost overlays | dirty cells and bands shown in the user's orientation, with snapping and per-element hints outside the format |
| shell project | the shell opens through the same project interface, keeping privileged hooks out of the toolkit |

## Related

- [Building a screen](../ui/Building-a-Screen.md)
- [Text and fonts](../gfx/Text-and-Fonts.md)
- [Render harness](../tools/Render-Harness.md)
- [Generated files](../../launcher/tools/gen/README.md)
- [Engine layer](Engine-Layer-Design-Sketch.md)
