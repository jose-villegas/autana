# Sand tools

Host-only scripts; the firmware build skips this folder. Each `report_*.sh`
names what it measures in its own header. The render harness the
`*_render_host.sh` scenes use is
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

## Grid fingerprint

`report_fingerprint.sh --check` compares both the normal simulation and a
host-only `SAND_FORCE_WORK` build against `fingerprint_baseline.txt`.
`SAND_SKIP_IF` evaluates the skip condition in both builds; forced work
always takes the work path. A forced mismatch identifies scenes where a
skip changes the output. The source-derived coverage report counts how
often each site would skip across the reference scenes. Zero counts warn
that a skip is untested and do not fail the check.

With no argument the tool prints both fingerprints. `--update` records
only the normal build and requires an accepted behaviour change.
`scripts/gates/check_skip_facts.py` requires declared facts to be read
through the gate, except in fact predicates and writers. Scheduling and
fact-maintenance functions retain their normal decisions.

A mismatch also exposes skipped RNG draws and pass-direction bookkeeping
such as `gas_flip` and `liquid_flip`. The rest predicate `cell_settled`
constrains crust formation. Hash differences require investigating these
effects alongside the correctness of absence facts.

## Host renders

```sh
./launcher/main/apps/sand/tools/sand_menu_render_host.sh    # title and options screens
./launcher/main/apps/sand/tools/brush_screen_render_host.sh # brush screen, both orientations
./launcher/main/apps/sand/tools/sand_sim_render_host.sh --video
```

`sand_sim_render_host.sh` steps the portable simulation through a volcano,
lake and grove with scripted tilt through the input filter, painting
through the shared material shading code into the host framebuffer. Its
landscape render is checked for size only (`|nopin`).
`make_volcano_clip.py` renders that scene, rotates each panel by its
scripted tilt angle, and encodes the loop as a GIF.

## Images in the docs

`doc_images.sh` here makes these in `docs/images/overview/`, run by
`launcher/tools/render/render_doc_images.sh`; see "Images in these docs" in
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

| Image | Shows |
|---|---|
| `sand-menu.png` | the title screen |
| `sand-simulation.gif` | the volcano, lake and grove inside its turning panel, made by `make_volcano_clip.py`; `--contact <path>` also writes a six-frame inspection strip |
