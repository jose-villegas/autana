# Editor

The host-side UI authoring tool. Its document model validates, saves and
bakes layouts. The window provides geometry canvases in both orientations;
project loading and registered draw entries are planned in
[`docs/plans/UI-Editor-Plan.md`](../docs/plans/UI-Editor-Plan.md).

The runtime links the public `ui/`, `draw/` and `gfx/` code with shared
packages. It links no shell or app source. SDL2, Dear ImGui and C++ are
host-only and never enter an ESP-IDF component graph.

## Layouts and the preview

| Data or code | Owner | Use |
|---|---|---|
| `<screen>_layout.json` | project folder | load and save an authored document |
| `LayoutDocument` | `src/layout_document` | validation, edits and header emission |
| `<screen>_layout_generated.h` | project build | static geometry for a screen |
| `runtime/runtime.c` | editor toolkit runtime | initialization, dimensions and tap constraints |
| orientation canvases | editor window | inspect and manipulate geometry |

- **The JSON is the source.** It names its screen and declares its elements
  (`id`, `label`, `interactive`), then gives one rect per element for
  `portrait` (panel width x height) and `landscape` (height x width). The editor
  writes it one rect per line, so an edit's diff is the rects that moved.
- **The header is output, never input.** `bake_header()` in
  `layout_document.cpp` emits firmware geometry. The editor calls it in-process
  on an explicit Bake; `editor_layout_bake` calls it from the command line.
  A device screen links the baked table: no JSON, no parser, no layout solver.
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
├── runtime/runtime.c         public ui/ + draw/ + gfx/ on the host; never an IDF component
└── tests/                    CTest: GoogleTest and C runtime tests
```

Project draw registration is planned. The launcher has no preview here; its
hand-written drawing belongs to `shell/ui/`.

## Adding a screen

The project interface will open a folder containing each screen's
`<screen>_layout.json` and registered draw entry. Draw entries take the same
layout value that the project build reads from its baked table. The first
authored document is the brush screen; the editor itself names no project
screen.

## Build

```sh
cmake -S editor -B editor/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build editor/build
ctest --test-dir editor/build --output-on-failure
```

To bake outside the window, run:

```sh
cd launcher
python tools/gen/bake_ui_layout.py <project>/<screen>_layout.json <project>/<screen>_layout_generated.h
```

`bake_ui_layout.py` configures its headless tree in `editor/build-bake` once and
builds `editor_layout_bake` incrementally on every run. Validation and emission
belong to C++. The generated-file drift gate checks each layout header against
its JSON through `bake_ui_layout.py`. Panel dimensions and tap constraints
come from the toolkit through `editor_runtime_*`. Invalid-document fixtures
pair a document with the error substring its rule must report.

The executable is `editor/build/autana_editor`. SDL2, Dear ImGui,
nlohmann/json and GoogleTest are fetched by git at pinned tags into the
untracked build
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
| `editor_layout_bake_c` (C) | the baker output compiles as C against `ui_layout.h` and matches the fixture rect |
| `editor_runtime_smoke` (C) | toolkit initialization and shared layout constraints |

The window itself - SDL and Dear ImGui glue - has no automated coverage.
Shell backdrop coverage is in `launcher/test/suites/suite_ridge_backdrop.c`.

```sh
python -m pip install gcovr==8.6
cmake -S editor -B editor/build-coverage -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DEDITOR_ENABLE_COVERAGE=ON
cmake --build editor/build-coverage --target editor_coverage
```

That runs the tests and fails under 80% line or 70% branch coverage of
`layout_document.cpp`, `core/edit_history.h` and `runtime.c`, writing
`coverage/index.html` and `coverage/coverage.xml` into the build directory.
