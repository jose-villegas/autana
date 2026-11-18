# Reference plan verification

Reference shards copy the plan's pose files byte for byte and render its recorded calls.
The workflow keeps shard artifacts with their producing plan, including across failed-job reruns.

| Check | Result |
|---|---|
| New plan/shard regressions | PASS on this implementation; FAIL with base `reference_rays` and `reference_shard` definitions |
| Real preparation, probe-only and path plus probes | PASS; the plan keeps all pre-render files and invokes no renderer |
| Shard matrix CLI and locked-set filtering | PASS |
| `python -m unittest scripts.ci.tests.test_runner_vendor` | PASS |
| `python scripts/gates/check_workflows.py` | PASS |
| `sh launcher/main/apps/sand/tools/report_fingerprint.sh --check` | PASS; baseline and forced-work fingerprints match |
| `python scripts/gates/check_comment_length.py --no-banners --limit 500 --header-limit 50 --top 0` | PASS |
| `python scripts/gates/check_style_audit.py --rule MODEL-NAMED-TOOL` | PASS |
| `bash launcher/test/run_tests.sh --sanitize` | PASS; pack suites waiting on the base lock are skipped |
| C formatting | No C or header files changed |
| `git diff --check` | PASS |
| Clone gate, CLI and staged-tree scan against the base | PASS; no growing clone pairs |

| New regression method | Existing owner |
|---|---|
| `test_planned_shards_render_one_selection_and_join_its_bytes` | `ProduceTests` |
| `test_poses_only_plan_writes_all_pre_render_files_without_rendering` | `ProduceTests` |
| `test_shard_refuses_a_plan_missing_its_set` | `ProduceTests` |
| `test_plan_keeps_the_real_preparation_files_and_does_not_render` | `PrepareReferencesTests` |

The first three fail against unchanged production code.
All four fail with the base functions loaded from `git show 9654d102494477010460d999253f4289150ca429:launcher/tools/bake/produce.py`.
The focused run passes all four on this implementation.

`python -m unittest launcher.tools.tests.test_bake` reports errors only in `TreeTests` and `SkipUnlockedTests`,
where `fetch_all` requires missing lock rows. `python launcher/tools/bake/bake.py check` also reports the stale lock.
`bake.py list --missing` is byte-identical between the base and this implementation; `launcher/bakes.lock` has no edits.

The new production constant is `produce.PLAN_MANIFEST`, beside the existing `SHARD_MANIFEST` owner.
The shard record describes completed renders; the plan record holds render calls and sample counts.

`reference_rays` owns poses-only preparation and ray counting.
`reference_shard` and `shard_renderer` own replay and partitioning.
No production functions or types are added.
