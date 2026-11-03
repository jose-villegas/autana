# Cluster level of detail

**Status:** approved; the host replay is built, the runtime is not.

The fitted mesh's welded positions are split into clusters, which are
grouped, simplified with each group's border locked, and split again until a
group no longer simplifies<sup>[[54]](../Citations.md#karis-2021)</sup>
(meshoptimizer's<sup>[[11]](../Citations.md#meshoptimizer)</sup> `clusterlod`). The finest
level holds the fitted mesh's triangles, but in `clusterlod`'s own clusters, not
the baked ones. Positions only, so a strip of one colour is not simplified on
its own; each level will get its own colour set.

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
| Levels in the baked mesh, pick in the cluster walk | planned `r3d_lod_pick()`, in `r3d_pipeline.c`; it then owns the pick and its constants, and `pick.py` calls it | not built |
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

**Open for the runtime:**

- Which clusters level 0 is: `clusterlod`'s, so the baked clusters change and
  every mesh is rebaked; or the baked ones, with the levels built over them.
- Whether the histogram is built in the frame it steers, one more pass over the
  clusters in view of every level, or from the last frame's, a frame late.

**Gate before the runtime ships:** with the levels a pose picks, no more of
the rest's hidden surface may show than at the finest level, and
[`test_sky_through_walls.py`](../../launcher/main/apps/render_lab/tools/tests/test_sky_through_walls.py)
must find no more holed frames than at the finest level.
