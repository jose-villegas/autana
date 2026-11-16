# Region-fit verification

| Check | Result |
|---|---|
| Import, probe and fitted-variant Python suites | PASS: `python -m unittest test_r3d_import test_r3d_fit_probes test_r3d_fitted_variant` (137 tests) |
| Reference-set Python suite | PASS (26 tests), including pinned reference packing |
| Mesh-bundle Python suite | PASS (29 tests), including held-out command wiring |
| Bake Python suite | Capture: 173 tests, 163 pass, nine bake-cache errors, one `shaped` fixture error; current StageTests PASS (10 tests) |
| Sanitized host suites | PASS: 3710 tests, zero failures or ignored tests |
| Sand fingerprint | PASS: baseline identical; forced work identical to normal |
| Clone gate | PASS on implementation commit `55d7d33d1` |
| Comment gate | PASS: `check_comment_length.py --no-banners --limit 500 --header-limit 50 --top 0` |
| Changed-file comment gate against task base | PASS: no changed C/C++ files |
| Documentation citations | PASS: no missing sections, documentation citations or paper problems |
| Model-named tool audit | PASS: no errors or warnings |
| Whitespace | PASS: `git diff --check` |
| C formatting | Not applicable: no C/C++ files changed |
| Bake lock | Unchanged |

`bake.py check` reports stale fitted-bake keys. Rebaking and lock changes are
outside this task's scope. The PR Bakes check supplies those artifacts.
The full bake-suite capture is
`/home/ville/autana-remote/engine/.dev/scripts/results/quiet/python-20261116-193938.log`.

The legacy `check-comment-length.sh --changed main` reports existing comment
violations outside this task's diff. The required explicit-limit gate passes.

## Regression evidence

All four tests fail against the task base's `import_settings.py` and
`appearance_simplify.py`, loaded from temporary files with current regression
tests. The worktree implementation stays intact.

| Test | Base result | Current result |
|---|---|---|
| `test_probe_fit_budget_matches_the_variant_triangles` | FAIL: SettingsError not raised | PASS; equal budget also loads |
| `test_geometry_budget_is_an_unknown_key` | FAIL: SettingsError not raised | PASS |
| `test_closure_allows_the_limit_and_refuses_one_pixel_more` | ERROR: missing closure constant | PASS |
| `test_main_checks_the_pruned_fit_before_writing` | FAIL: one hole count; excessive holes accepted | PASS; failure writes no mesh |

Red capture: `/home/ville/autana-remote/engine/.dev/scripts/results/quiet/heny7-red-20261116-194450.log`.

## New owners

`check_closure` and `CLOSURE_GROWTH` belong beside `hole_pixels` in
`appearance_simplify.py`. The inspected `check_hidden_copies` owns copy occlusion;
its per-pose mean/worst limits do not express aggregate hole growth.

`ClosureTests` and its two methods use the existing probe suite, mesh fixture,
and mock loaders. Its local `write_mesh` fake observes output without a GPU.
The two new settings tests extend `SceneTests` and `ImportTests` respectively.
No production type or duplicate mesh, raster or settings helper is added.
