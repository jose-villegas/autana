# Floor captures

The aligned sheet in [compare.png](compare.png) shows the float reference,
firmware drawing and an amplified difference at the frame times in
[summary.txt](summary.txt). [Crops](compare.crops.png) enlarge the strongest
pixel differences. The direct height reference and its error bound live in
`test_baked_floor_matches_direct_float`.

The reference is the float prototype the look was approved from; it is not
kept in the repository. Alignment uses the result's camera
track and photograph start, starts the reference swell at `title_start_ms`,
sets its opacity ceiling to full strength and disables time tint. These are
reference controls; the packet formula and float projection remain the
prototype's.

[measurements.json](measurements.json) records QEMU instruction counts for
isolated `draw_floor` calls, their source hashes and deltas against the base
floor and main's rings. Each row is virtual instruction cost, not board time.
Main's rings use their own camera and timeline; their photograph is fully
revealed at the last row, where the suite still calls the floor for a kernel
comparison. The capture suite is `run_boot_floor_cost_suite`.
