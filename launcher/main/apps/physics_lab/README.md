# Physics Lab

A development-only lab for physics components. Scenes are added per component;
the empty stage holds the UI background.

| Console command | Result |
|---|---|
| `physics scenes` | List the scenes |
| `physics scene stage` | Show the empty stage |

| File / suite | Purpose |
|---|---|
| `physics_presets.h` | `PHYSICS_PRESETS(X)`, the empty preset table; `PHYSICS_PRESET_COUNT` derives from its rows |
| `run_physics_lab_suite` | Stage entry, presented frames, console switching and memory cleanup |
| `run_physics_presets_suite` | One timed test per preset, plus the table consistency test; included in perf-scoped images |

Add a row as `X(ID, key, duration_ms)` with a fixed load and duration. Define
`scene_physics_ID` and declare it for the app's scene table. The suite expands
the same row into a scene visit and a `frame_profile:` timing log.
