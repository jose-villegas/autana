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
| `run_physics_presets_suite` | One timed test per preset, plus a check that the scene table holds the stage and each preset; included in perf-scoped images |

A row's fields are described in `physics_presets.h`. Adding one also means
defining its scene, `scene_physics_<ID>`; the suite then times it and logs a
`frame_profile:` line named by the panel's orientation.
