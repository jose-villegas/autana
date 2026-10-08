# Editor

The host-side authoring tool. It edits a firmware screen's layout and previews
it **through the firmware's own UI and gfx code**, compiled for the host, so
what it shows is what the device draws. UI layout is its first module; the
plan is [`docs/plans/UI-Editor-Plan.md`](../docs/plans/UI-Editor-Plan.md).

The device runtime stays under `launcher/`. SDL2, Dear ImGui and C++ are
host-only and never enter an ESP-IDF component graph.

## How a screen flows through it

```mermaid
flowchart LR
    JSON["launcher/main/ui/<br/>&lt;screen&gt;_layout.json"] -->|load / save| DOC["LayoutDocument<br/><i>src/layout_document</i>"]
    DOC -->|rects, every edit| RT["editor_runtime_render()<br/><i>runtime/runtime.c</i>"]
    RT -->|"real ui/ + gfx/"| PREVIEW["both orientations,<br/>side by side"]
    DOC -->|"Bake: bake_header()"| HEADER["&lt;screen&gt;_layout_generated.h"]
    HEADER --> FW["firmware build"]
```

- **The JSON is the source.** It names its screen and declares its elements
  (`id`, `label`, `interactive`), then gives one rect per element for
  `portrait` (panel width x height) and `landscape` (height x width). The editor
  writes it one rect per line, so an edit's diff is the rects that moved.
- **The header is output, never input.** `bake_header()` in
  `layout_document.cpp` emits firmware geometry. The editor calls it in-process
  on an explicit Bake; `editor_layout_bake` calls it from the command line.
  The device links the baked table: no JSON, no parser, no layout solver.
- **Save and Bake are separate**, so a preview edit cannot silently change a
  device build. Both refuse a layout with problems.
- **One rule set.** Inside the canvas, no overlap, and at least
  `UI_TAP_MIN` in each dimension for an `interactive` element. `LayoutDocument`
  owns these rules for loading, dragging, saving and baking.

## Layout

```
editor/
├── src/
│   ├── main.cpp              the window: hierarchy, previews, inspector, problems
│   ├── layout_document.*     one document type for every authored screen
│   └── core/                 reusable: dockspace, edit history, RGB565 texture
├── include/editor/runtime.h  the C boundary the editor renders through
├── runtime/runtime.c         firmware ui/ + gfx/ on the host; never an IDF component
└── tests/                    CTest: GoogleTest and C runtime tests
```

Control Center is the authored document. The launcher is listed beside it as
a preview only: it is a flowed scrolling list, which a table of fixed rects
cannot express.

## Adding a screen

1. Write `launcher/main/ui/<screen>_layout.json` and bake it.
2. Give the screen a draw function that takes the baked struct, split from
   its frame the way `ui_control_center_draw.c` is.
3. Add the screen to `editor_screen_t` and to `runtime.c`'s render and
   element-count switch, and its sources to `editor_runtime` in
   `CMakeLists.txt`.
4. Load its document in `load_system_screens()` in `main.cpp`.

## Build

```sh
cmake -S editor -B editor/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build editor/build
ctest --test-dir editor/build --output-on-failure
```

To bake outside the window, run:

```sh
cd launcher
python tools/gen/bake_ui_layout.py main/ui/<screen>_layout.json main/ui/<screen>_layout_generated.h
```

The launcher configures its headless tree in `editor/build-bake` once and
builds `editor_layout_bake` incrementally on every run. Validation and emission
belong to C++. The generated-file drift gate checks each layout header against
its JSON through this launcher. Panel dimensions and tap constraints come
from the firmware through `editor_runtime_*`. Invalid-document fixtures pair a
document with the error substring its rule must report.

The executable is `editor/build/autana_editor`. SDL2, Dear ImGui, nlohmann/json and
GoogleTest are fetched by git at pinned tags into the untracked build
directory. `-DEDITOR_BUILD_GUI=OFF` builds and tests everything except the
window and fetches neither SDL2 nor Dear ImGui; CI uses it
(`.github/workflows/host-tests.yml`).

On Windows with MinGW the binaries link statically: a MinGW executable
otherwise loads whichever `libstdc++-6.dll` `PATH` reaches first, and Git for
Windows ships an incompatible one.

## Tests and coverage

CTest is the single entry point.

| suite | covers |
|---|---|
| `editor_document.*` (GoogleTest) | load, validate, save, bake, wrong rect count; fixtures assert their named rule; JSON re-serialises byte for byte |
| `editor_core.*` (GoogleTest) | edit history: undo, redo, saved revision |
| `editor_navigation.*` (GoogleTest) | the firmware's `system_navigation.c`, all four rotations |
| `editor_runtime_smoke` (C) | both screens and orientations render; an authored rect reaches the pixels; bad layouts are refused |
| `editor_ridge_backdrop` (C) | the launcher over its ridge: idle sends nothing, a touch wakes it, and at rest the screen is the settled one exactly |

The window itself - SDL and Dear ImGui glue - has no automated coverage.

```sh
python -m pip install gcovr==8.6
cmake -S editor -B editor/build-coverage -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DEDITOR_ENABLE_COVERAGE=ON
cmake --build editor/build-coverage --target editor_coverage
```

That runs the tests and fails under 80% line or 70% branch coverage of
`layout_document.cpp`, `core/edit_history.h`, `runtime.c` and
`system_navigation.c`, writing `coverage/index.html` and `coverage/coverage.xml`
into the build directory.
