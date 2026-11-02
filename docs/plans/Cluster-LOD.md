# Cluster level of detail

**Status:** approved; the host replay is built, the runtime is not.

The baked mesh's clusters are grouped, simplified with each group's border
locked, and split again until a group no longer simplifies<sup>[[54]](../Citations.md#karis-2021)</sup>
(meshoptimizer's<sup>[[11]](../Citations.md#meshoptimizer)</sup> `clusterlod`). The finest
level is the fitted mesh itself. Coarser levels simplify its welded positions
only, so a strip of one colour is not simplified on its own; each level will
get its own colour set.

**The pick.** A cluster is drawn when its own error, projected at its sphere,
is at most the tolerance in pixels and its group's error is above it. Every
cluster of a group shares the group's sphere and error, so neighbours change
level together and leave no crack. The rest mesh is one hierarchy, its groups
spanning objects where they weld; each copy shape has its own, shared by its
copies.

**The budget.** One tolerance for the whole frame keeps the cut crack-free.
It is also the cut that meets a triangle budget at the least error<sup>[[110]](../Citations.md#wimmer-1998)</sup>.
The run-time pick finds it from a histogram of 32 tolerance bins. Each
cluster in view adds its triangles at the bin where it starts to be drawn and
removes them at the bin where its group takes over. The prefix sum is then the
triangle count at every bin edge. The tolerance never goes below half a
pixel<sup>[[111]](../Citations.md#hoppe-1997)</sup>.

| Piece | Where | State |
|---|---|---|
| Levels of a mesh | [`lod/levels.py`](../../launcher/tools/r3d/lod/levels.py), [`lod/clusterlod.cpp`](../../launcher/tools/r3d/lod/clusterlod.cpp) | host |
| Pick, histogram, exact tolerance | [`lod/pick.py`](../../launcher/tools/r3d/lod/pick.py) | host |
| Replay over a camera path | [`lod/lod_replay.py`](../../launcher/tools/r3d/lod/lod_replay.py) | host |
| Levels in the baked mesh, pick in the cluster walk | `r3d_lod_pick()` | not built |
| Tolerance per pose baked for the path; the fit trained on it | bake stage | not built |

```sh
python launcher/tools/r3d/lod/lod_replay.py SCENE.scene.toml --object NAME --out DIR [--budgets N,...] [--segment T0 T1]
```

It reports, per pose and over the path, the finest level's triangles in view,
what fixed tolerances draw, and for each budget:
- the histogram's tolerance against the exact one;
- the triangles drawn at each;
- the terminal share: drawn triangles whose group cannot be simplified
  further, which no budget removes.

**Gate before the runtime ships:** with the levels a pose picks, no more of
the rest's hidden surface may show than at the finest level. The
sky-through-walls test must not get worse.
