# Boot floor instruction costs

The rows below measure the linked floor renderer under QEMU `--icount`.
They count instructions, not ESP32-S3 elapsed time. Main's rings are from
`origin/main` at `5394e1a7d`; its checkout has only the measurement suite added.
The mesh comparison starts from `04d1a506b`. The compiler and emulator are the
versions selected by `launcher/idf-pin.json` and the normal QEMU runner.

## Settings

The shipped timeline keeps `mesh_cells: 48`, `mesh_dash: 0`,
`mesh_cull_alpha: -1`, and `mesh_far_stride: 1`.

- `dash`: `mesh_dash: 1` skips alternating mesh edges.
- `cull`: `mesh_cull_alpha: 2` rejects dim edges and edges with a common
  screen outcode before their colour and line setup. A conservative radial
  bound skips a node's lift, transform and projection only when every
  incident edge is below the fade threshold. This bound uses floor fade,
  independently of the frame's ink.
- `far`: `mesh_far_stride: 2` retains full column density and spaces rows
  twice as far apart outside the central half of the row-index range. Every
  column connects to the previous retained row, including at the density
  transition. The cells are rectangular there; no seam needs stitching.
- `cells32`: `mesh_cells: 32` changes both lattice dimensions. The generator
  derives the cell step from the fade's reach.
- `cull-far` and `cull-32` combine those settings.

## Measurements

The host scans every integer millisecond of the timeline. The mesh-only
probe, with spoke reveal starting beyond the visible timeline, finds its
first maximum at **3724 ms**, with **3560 mesh edges**. The ordinary floor
adds **64 spoke calls** there. Crossfade start is the existing performance
checkpoint, `BOOT_ANIM_IMAGE_START_MS + 50`, or **3550 ms** for this timeline.
Every variant and main's rings use these same two timestamps.

Nodes are a host reference count derived from the retained row lattice and
its conservative fade bound, including attempted projections rejected by the
near plane. Edges are observed `draw_line_ex` calls,
including spokes and calls which screen clipping rejects. The host linker
wrapper forwards every call unchanged. Pixel writes count the clipped
Bresenham walk, including repeated writes to shared endpoints; they are not
unique covered pixels.

**Projection + rejection** is the measured ink-0 floor run. It includes
node construction, row/edge traversal, alpha rejection and the black spoke
rasterization that ink 0 still performs. It is an upper bound on projection,
not an exclusive projection timer. The real-ink **Total** is independently
measured through the linked production `draw_floor()`.

**Setup est.** and **Pixels est.** use production `draw_line_ex` microbenchmarks:
one on-screen point and one horizontal line of `FLOOR_LINE_LENGTH` pixels.
The difference divided by the additional pixels gives instructions/write;
subtracting one write from the point gives instructions/call. Each variant
uses its own calibration. Setup includes draw acquisition, clipping and
marking; the pixel estimate includes the walk and stores. These coefficients
approximate varied slopes, clipped and rejected edges, and marking extents.

**Gap** is `Total - ink0 - Setup est. - Pixels est.`. It retains colour work,
edge preparation and differences between the line model and the real edges.
It also reflects spoke rasterization already charged to ink 0. The estimated
columns are not an exact additive attribution of the measured total.

<!-- measured-table -->
| Variant | ms | Nodes (ref) | Edges | Pixel writes | Projection + rejection | Setup est. | Pixels est. | Total | Delta main | Delta % | Gap |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| main rings | 3550 | - | - | - | - | - | - | 2,305,781 | 0 | 0.0% | - |
| main rings | 3724 | - | - | - | - | - | - | 2,151,625 | 0 | 0.0% | - |
| default | 3550 | 1,941 | 3,584 | 18,994 | 1,961,609 | 1,131,495 | 780,155 | 4,153,359 | +1,847,578 | +80.1% | +280,100 |
| default | 3724 | 1,941 | 3,624 | 18,733 | 1,965,703 | 1,144,123 | 769,435 | 4,154,047 | +2,002,422 | +93.1% | +274,786 |
| dash | 3550 | 1,941 | 1,825 | 9,692 | 1,837,891 | 576,110 | 398,087 | 2,935,922 | +630,141 | +27.3% | +123,834 |
| dash | 3724 | 1,941 | 1,844 | 9,522 | 1,842,062 | 582,108 | 391,104 | 2,935,859 | +784,234 | +36.4% | +120,585 |
| cull | 3550 | 1,861 | 2,326 | 18,349 | 2,046,062 | 734,408 | 753,645 | 3,880,609 | +1,574,828 | +68.3% | +346,494 |
| cull | 3724 | 1,861 | 2,411 | 18,140 | 2,052,766 | 761,246 | 745,060 | 3,886,188 | +1,734,562 | +80.6% | +327,115 |
| far | 3550 | 1,613 | 2,919 | 17,753 | 1,625,875 | 921,373 | 729,174 | 3,524,344 | +1,218,562 | +52.8% | +247,922 |
| far | 3724 | 1,613 | 2,936 | 17,564 | 1,629,109 | 926,739 | 721,411 | 3,526,719 | +1,375,094 | +63.9% | +249,460 |
| cells32 | 3550 | 897 | 1,619 | 12,152 | 918,281 | 511,233 | 499,099 | 2,049,672 | -256,109 | -11.1% | +121,059 |
| cells32 | 3724 | 897 | 1,632 | 11,963 | 921,781 | 515,338 | 491,336 | 2,041,297 | -110,328 | -5.1% | +112,841 |
| cull-far | 3550 | 1,573 | 2,007 | 17,202 | 1,723,547 | 633,624 | 706,551 | 3,357,922 | +1,052,141 | +45.6% | +294,200 |
| cull-far | 3724 | 1,573 | 2,098 | 17,071 | 1,730,844 | 662,354 | 701,170 | 3,379,203 | +1,227,578 | +57.1% | +284,836 |
| cull-32 | 3550 | 861 | 1,080 | 11,800 | 967,391 | 340,964 | 484,665 | 1,942,938 | -362,844 | -15.7% | +149,918 |
| cull-32 | 3724 | 861 | 1,112 | 11,651 | 972,203 | 351,067 | 478,545 | 1,936,766 | -214,859 | -10.0% | +134,951 |

- default: setup 315.707 instructions/call, pixels 41.074 instructions/write
- dash: setup 315.677 instructions/call, pixels 41.074 instructions/write
- cull: setup 315.739 instructions/call, pixels 41.073 instructions/write
- far: setup 315.647 instructions/call, pixels 41.073 instructions/write
- cells32: setup 315.771 instructions/call, pixels 41.071 instructions/write
- cull-far: setup 315.707 instructions/call, pixels 41.074 instructions/write
- cull-32: setup 315.708 instructions/call, pixels 41.073 instructions/write
<!-- /measured-table -->

At 3550 ms, the default mesh's projection/rejection row is 47.2% of its
measured total. Estimated line setup is 27.2%, and estimated pixel work is
18.8%. The gap is 6.7% of the total. Across the table, the line model accounts
for approximately 81-90% of the measured total-minus-ink-0 work; the positive
gaps keep that approximation visible.

Culling plus 32 cells is the cheapest measured variant at both checkpoints:
15.7% below rings at 3550 ms and 10.0% below rings at 3724 ms. Culling alone
reduces calls substantially, but its projection/rejection row is larger than
the default mesh's because the per-node fade bounds and per-edge outcodes
also cost instructions. Reducing node density has the stronger measured
benefit. Board elapsed time is a separate measurement. The enabled-variant complexity
scan has a maximum score of 14, below the per-function limit of 15.

The default QEMU call-graph stack check uses 3008 of the main task's 3648
bytes, leaving 640 bytes against the 512-byte reserve. With culling and far-row
spacing enabled at full cell count, it uses 3056 bytes and leaves 592 bytes. The boot chain is
`boot_anim_run > boot_anim_draw_frame > draw_scene > draw_floor > draw_mesh_edge > draw_line_ex > line_walk > gfx_mark_drawn > dirty_mark`.

## Visual comparisons

Each sheet has rows for 1510, 2650 and 3550 ms: default mesh, variant, and
amplified difference. The linked crops enlarge the strongest differences.
The 32-cell crops show larger, more angular lifted cells throughout the
mesh. Far-row spacing keeps the central weave and removes outer lines;
the retained lines remain connected across its density transition.

- [Dashed edges](floor_cost/dash.png), [crops](floor_cost/dash.crops.png).
- [Culling](floor_cost/cull.png), [crops](floor_cost/cull.crops.png).
- [Far rows](floor_cost/far.png), [crops](floor_cost/far.crops.png).
- [32 cells](floor_cost/cells32.png), [crops](floor_cost/cells32.crops.png).
- [Culling and far rows](floor_cost/cull-far.png), [crops](floor_cost/cull-far.crops.png).
- [Culling and 32 cells](floor_cost/cull-32.png), [crops](floor_cost/cull-32.crops.png).

The render folders are `launcher/tools/results/boot-floor/{default,dash,cull,far,cells32,cull-far,cull-32}`.
They retain the renderer, asset packs and individual BMPs. Raw measurement
captures are under `launcher/tools/results/boot-floor/measurements/`.

## Regeneration

Run from the repository root in Git Bash. Initialize the host build with
`bash launcher/test/run_tests.sh`; its makefile and asset packs are reused
by `--counts`. The tool restores the timeline JSON and header byte-for-byte
on exit. Run one instance at a time because it changes these shared inputs.

```sh
variants=(
  --variant 'dash:{"mesh_dash":1}'
  --variant 'cull:{"mesh_cull_alpha":2}'
  --variant 'far:{"mesh_far_stride":2}'
  --variant 'cells32:{"mesh_cells":32}'
  --variant 'cull-far:{"mesh_cull_alpha":2,"mesh_far_stride":2}'
  --variant 'cull-32:{"mesh_cull_alpha":2,"mesh_cells":32}'
)
python launcher/tools/boot_anim/compare_floor_variants.py \
  --out launcher/tools/results/boot-floor --frame 1510 --frame 2650 --frame 3550 \
  "${variants[@]}"
python launcher/tools/boot_anim/compare_floor_variants.py \
  --out launcher/tools/results/boot-floor/measurements --counts --icount \
  "${variants[@]}"
```

For the rings baseline, make a checkout outside `launcher/` so recursive
tool and stack scans cannot read a nested repository:

```sh
GIT_LFS_SKIP_SMUDGE=1 git worktree add --detach build/main-rings 5394e1a7d
cp launcher/test/suites/suite_boot_floor_icount.c build/main-rings/launcher/test/suites/
(cd build/main-rings && bash launcher/test/run_qemu_tests.sh \
  --suite run_boot_floor_cost_suite --icount --timeout 900)
cp build/main-rings/launcher/build.qemu.shell/qemu_serial.log \
  launcher/tools/results/boot-floor/measurements/main-rings.log
python launcher/tools/boot_anim/compare_floor_variants.py \
  --out launcher/tools/results/boot-floor/measurements \
  --main-log launcher/tools/results/boot-floor/measurements/main-rings.log \
  --report launcher/tools/results/boot-floor/measurements/table.md "${variants[@]}"
```

The mesh-only maximum can be regenerated without changing the sampler:

```sh
python launcher/tools/boot_anim/compare_floor_variants.py \
  --out launcher/tools/results/boot-floor/mesh-only --counts \
  --variant 'mesh-only:{"grid_spoke_start_ms":5500,"grid_spoke_draw_ms":0}'
```

`run_boot_floor_cost_suite` is an on-request exploratory suite. Run it
explicitly with `--icount --suite run_boot_floor_cost_suite`; its name does
not match the hot-kernel ratchet's discovery suffix, and frame totals have
no ratchet baseline.

The table command uses `quality/icount_gate.py::parse_log` to convert the
runner's virtual microseconds to instructions. `build/c_header.py::read_define`
reads the suite's frame and line-length constants. The comparison images use
`tools/render/render_compare.py`; `--crops` is enabled by the variant tool.

## Measurement owners

The production owners are `mesh_node_t`, `mesh_node`, `draw_mesh_edge`,
`draw_mesh_row` and `draw_floor`; they hold filtering and row spacing.
The existing `gfx_box_outcode` owns screen rejection, and `mathi_isqrt64`
owns radial bounds. Generated `BOOT_ANIM_MESH_CULL_ALPHA` and
`BOOT_ANIM_MESH_FAR_STRIDE` belong to the timeline generator's existing
setting list.

The measurement functions belong to `suite_boot_floor_icount.c`:
`__wrap_draw_line_ex` observes calls via the host runner's existing linker-wrap
mechanism; `fixture` resets its counters; `node_projects` and
`projected_nodes` count the retained lattice (the latter has a rings fallback);
`count_frame` collects a frame; `test_floor_timeline_edge_counts` scans the
timeline; `report_total` times the existing floor entry point;
`report_line_cost` calibrates the existing line primitive;
`test_floor_instruction_rows` drives QEMU rows; and
`run_boot_floor_cost_suite` registers the on-request suite. No existing
suite scans this floor or counts its primitive calls. The suite's
`FLOOR_CROSSFADE_OFFSET_MS`, `FLOOR_EDGE_PEAK_MS`, `FLOOR_TIMING_REPS`,
`FLOOR_LINE_REPS`, `FLOOR_LINE_COLOR`, `FLOOR_LINE_LENGTH`, `FLOOR_LINE_X` and
`FLOOR_LINE_Y` own its checkpoints and calibration workload.
`FLOOR_COUNT_PREFIX`, `FLOOR_ROW_PREFIX`, `FLOOR_TOTAL_ROW`,
`FLOOR_REJECT_ROW`, `FLOOR_POINT_ROW` and `FLOOR_LONG_ROW` own the report
keys; the Python reporter reads them from this suite. The existing phase
profiler's `build_checkpoints` is file-local, so the suite derives the
crossfade checkpoint from the public timeline start.

`compare_floor_variants.py` owns `run`, `render`, `measure`, `cost_table` and
`main`: the render script builds a single timeline, the QEMU runner
executes one image, and the performance report parses board phase timings.
None cycles JSON variants or reconciles this host counter with instruction
rows. It delegates rendering, image comparison and instruction parsing to
those existing owners, and make selection to `build/host_make.sh`. `test_mesh_cost_settings_validate_ranges` and
`test_mesh_cost_settings_are_generated` validate the timeline generator
with acceptance and rejection cases for culling and row spacing.
`test_floor_cost_suite_is_explicit_only` verifies on-request registration
and exclusion from QEMU ratchet discovery.
