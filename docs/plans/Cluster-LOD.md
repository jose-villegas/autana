# Cluster level of detail

**Status:** approved; the host replay, the baked levels and the pick at a fixed tolerance are built; the budget is not.

The fitted mesh's welded positions are split into clusters, which are
grouped, simplified with each group's border locked, and split again until a
group no longer simplifies<sup>[[54]](../Citations.md#karis-2021)</sup>
(meshoptimizer's<sup>[[11]](../Citations.md#meshoptimizer)</sup> `clusterlod`). The finest
level holds the fitted mesh's triangles in `clusterlod`'s own clusters, and the
bake draws those as its level 0. A fit starts from a mesh without levels. A CPU levels stage builds the
hierarchy from its fitted bytes. Positions only, so a strip of one colour is not
simplified on its own; each coarser cluster samples its colours from the
nearest level-0 surface facing its way.

**The bake.** [`lod/bake_levels.py`](../../launcher/tools/r3d/lod/bake_levels.py)
builds one hierarchy per sidedness, with the positions the two share locked. It
pushes each coarse vertex inside a group's border outward, up to the group's
error, so a coarse level never sits inside the finer surface it replaces.
Group spheres and errors are rounded outward into position ticks; the LLOD
format ([LLOD.md](../assets/formats/LLOD.md)) states their ordering rules.
The cluster tree's root has two children: an octree over level 0 alone,
built as a mesh without levels builds its own, and a run of the coarse
clusters in the order of the group simplified into each. The cull walks
level 0's octree, then takes the cut's coarse clusters from each coarse
group's run, so a frame never visits a coarse cluster it does not draw. The
`[geometry] lod` import setting turns levels off; level 0 is then the baked
meshlets.

**The pick.** A cluster is drawn when its own error, projected at its sphere,
is at most the tolerance in pixels and its group's error is above it. Every
cluster of a group shares the group's sphere and error, so neighbours change
level together and leave no crack. The rest mesh is one hierarchy, its groups
spanning objects where they weld; each copy shape has its own, shared by its
copies.

**The budget.** One tolerance for the whole frame keeps the cut crack-free.
It is also the cut that meets a triangle budget at the least error: the
allocation Wimmer and Schmalstieg derive per object<sup>[[110]](../Citations.md#wimmer-1998)</sup>,
here over clusters. The run-time pick will find it from a histogram of
log-spaced tolerance bins ([`pick.py`](../../launcher/tools/r3d/lod/pick.py)).
Each cluster in view adds its triangles at the bin where it starts to be drawn
and removes them at the bin where its group takes over. The prefix sum is then
the triangle count at every bin edge. Culling a coarser cluster's box can keep
it where its finer clusters were culled, so the count can rise with the
tolerance; the histogram then lands at or after the least tolerance within
budget, still within it. Refining by screen-space error is
Hoppe's<sup>[[111]](../Citations.md#hoppe-1997)</sup>; the floor of half a pixel is this tree's choice.

| Piece | Where | State |
|---|---|---|
| Levels of a mesh | [`lod/levels.py`](../../launcher/tools/r3d/lod/levels.py), [`lod/clusterlod.cpp`](../../launcher/tools/r3d/lod/clusterlod.cpp) | host |
| Pick, histogram, exact tolerance | [`lod/pick.py`](../../launcher/tools/r3d/lod/pick.py) | host |
| Replay over a camera path | [`lod/lod_replay.py`](../../launcher/tools/r3d/lod/lod_replay.py) | host |
| Levels in the baked mesh | [`lod/bake_levels.py`](../../launcher/tools/r3d/lod/bake_levels.py), [`r3d_lit_mesh.h`](../../launcher/main/render/r3d_lit_mesh.h) | built |
| Pick at a tolerance, with hysteresis and a coverage cap, once a frame in output pixels | [`r3d_lod_pick()`](../../launcher/main/render/lod/r3d_lod.h), run by `raster_pick_levels()`; its coarse bits live per scene renderer, and the cluster walk only reads them | built |
| Histogram from the last frame, steering the tolerance to a budget | `r3d_lod.c` | not built |
| Tolerance per pose baked for the path; the fit trained on it | bake stage | not built |

```sh
python launcher/tools/r3d/lod/lod_replay.py SCENE.scene.toml --object NAME --out DIR [--budgets N,...] [--segment T0 T1]
```

It writes poses.csv, summary.md and lod.png to DIR; the numbers come from
running it on a bake. It reports, per pose and over the path, the finest level's triangles in view,
what fixed tolerances draw, and for each budget:
- the histogram's tolerance against the exact one;
- the triangles drawn at each;
- the terminal share: drawn triangles whose group cannot be simplified
  further, which no budget removes.

**Decided for the runtime:** level 0 is `clusterlod`'s clusters, so every
mesh is rebaked; the histogram is built from the last frame's clusters in view,
a frame late.

**The pick's hysteresis.** A group turns coarse once its projected error falls
`R3D_LOD_HYSTERESIS_PX` below the tolerance and turns fine again once it
rises that far above, so a still camera never flickers between levels. A
coarse group's parent bounds hold its own, so its finer groups are always
coarse too; a closing pass sets coarse any source float rounding left fine
far off, so the cut stays crack-free whatever the floats do.

**The coverage cap.** A group whose radius, projected where its error is,
spans more than `R3D_LOD_COVERAGE_PERMILLE` thousandths of the picture's
height stays fine, whatever its error. A large coarse group shows its
simplified surface over much of the screen, and its colours, sampled from the
nearest level-0 surface, shimmer under a slow camera drift where the
geometric error alone says nothing changed. A parent's bounds hold its
sources', so it never covers less than one of them, and the cap keeps a
coarse parent's sources coarse. The table renders each cap along the paths
and scores it against cap 0, the finest level. Blocks popping is
frame-to-frame change beyond the finest level's own (render_temporal.py's
measure); triangles are those submitted, against the finest level's.

<!-- generated: lod-coverage sha256=372d60bdcc88a87a235f4048ca763ca61047b63cc4d8fab7b8af4fa800f77ad4 -->
| Path | Cap | Blocks popping | Mean difference /255 | Triangles, mean | Triangles, p95 |
|---|---|---|---|---|---|
| Sponza | none | 0.0906% | 0.177 | -12.3% | -14.0% |
| Sponza | 50% | 0.0637% | 0.062 | -11.1% | -10.6% |
| Sponza | 25% | 0.0637% | 0.062 | -11.1% | -10.6% |
| Sponza | 10% | 0.0588% | 0.053 | -10.5% | -10.6% |
| Bistro | none | 0.2961% | 0.243 | -14.5% | -17.3% |
| Bistro | 50% | 0.2726% | 0.215 | -13.6% | -15.8% |
| Bistro | 25% | 0.1760% | 0.108 | -10.9% | -15.1% |
| Bistro | 10% | 0.0086% | 0.005 | -2.4% | -3.7% |
<!-- /generated: lod-coverage -->

```sh
python launcher/tools/r3d/lod/coverage_table.py --script launcher/main/apps/render_lab/tools/render_lab_render_host.sh \
    --render Sponza "--scene sponza-fitted-full --no-hud --frames 1101 --dt 33" \
    --render Bistro "--scene bistro-fitted-full --no-hud --frames 900 --dt 33" --doc docs/plans/Cluster-LOD.md
```

**Gate before the runtime ships:** with the levels a pose picks, no more of
the rest's hidden surface may show than at the finest level, and
[`test_sky_through_walls.py`](../../launcher/main/apps/render_lab/tools/tests/test_sky_through_walls.py)
must find no more holed frames than at the finest level.
