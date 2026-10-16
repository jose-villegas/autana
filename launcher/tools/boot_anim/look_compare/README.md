# Floor captures

The aligned sheet in [compare.png](compare.png) shows the float reference,
firmware drawing and an amplified difference at the frame times in
[summary.txt](summary.txt). [Crops](compare.crops.png) enlarge the strongest
pixel differences. The direct height reference and its error bound live in
`test_baked_floor_matches_direct_float`.

Reference source: local commit `16aaa2b22`. Alignment uses the result's camera
track and photograph start, starts the reference swell at `title_start_ms`,
sets its opacity ceiling to full strength and disables time tint. These are
reference controls; the packet formula and float projection remain the
prototype's. [prototype.png](prototype.png) keeps the unaligned comparison.

Local whole-animation artifacts are under `launcher/test/build/look/`:
`animation.avi` is the firmware's 184 frames at 30 ms steps and quarter turn 1;
`compare/animation.mp4` is the unaligned prototype, firmware and per-frame
heatmap side by side. `compare/animation.frames.csv` holds per-frame scores.
They are generated artifacts outside the tracked source.

[measurements.json](measurements.json) records QEMU instruction counts for
isolated `draw_floor` calls, their source hashes and deltas against the base
floor and main's rings. Each row is virtual instruction cost, not board time.
Main's rings use their own camera and timeline; their photograph is fully
revealed at the last row, where the suite still calls the floor for a kernel
comparison. The capture suite is `run_boot_floor_cost_suite`.

[symbols.json](symbols.json) identifies the added interfaces and their owners.
