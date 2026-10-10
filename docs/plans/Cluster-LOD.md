# Cluster level of detail

**Status:** built offline on the branch `feature/r3d-cluster-lod`, not
merged; no runtime reads it.

The baked mesh's meshlets can be grouped, simplified with each group's border
locked, and split again until one is left<sup>[[54]](../Citations.md#54)</sup> (meshoptimizer's<sup>[[11]](../Citations.md#11)</sup> `clusterlod`
example), so a runtime draws coarser clusters where they are smaller than a
pixel. The branch holds that bake, a normal cone per cluster, and a host tool
that counts what a pick would draw at a camera's poses and diffs the pixels.

**The pick.** Each cluster stores its own error and its parent group's, each
with a sphere. It is drawn when its own error, projected at its sphere, is at
most the tolerance in pixels and its parent's is above it. Groups share one
sphere and error, so neighbours picked at different levels meet along vertices
they share and leave no crack.

**What it saved.** On a lit, already-simplified interior of about 17k
triangles, drawn at 184 x 224 along a flythrough:

| Tolerance | Drawn triangles saved | Pixels that differ |
|---|---|---|
| 1 px | 4.9% | 0.5% |
| 4 px | 17-20% | 0.6-2.5% |

The mesh was already reduced to the picture's budget, so little was sub-pixel.
The levels roughly doubled the mesh's flash. A denser mesh, or one drawn
smaller, would save more; the branch's tool answers that for any baked mesh.

**Why it is parked.** Enclosed interiors, such as the measured atrium or a
cathedral, never put a cluster far enough away for a coarser level to pay:
the walls bound the view. LOD is for scenes with a far plane. Revisit it when
an open scene with distant geometry exists.

**Not done.** A runtime that picks per cluster, and normal-cone culling, which
the branch bakes but nothing reads.
