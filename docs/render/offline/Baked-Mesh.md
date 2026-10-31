# Baked Mesh

The mesh a bake writes and a renderer reads: its clusters and tree, and the
meshlets the clusters are. Its entry is [LMSH](../../assets/formats/LMSH.md); a shared shape's
colours are [LCOL](../../assets/formats/LCOL.md) sets. How a mesh is made is
[Mesh-Import.md](Mesh-Import.md); drawing it is [Mesh-Rendering.md](../Mesh-Rendering.md).

## Clusters and the tree

Triangles group into **clusters**, the leaves of a tree rooted at `nodes[0]`:
one box test culls a subtree. Positions are `int16`, `position_scale` per unit.

![A baked mesh's clusters, then the tree level by level with one root-to-leaf path bold](../../images/render/baked-mesh-tree.gif)

*Clusters, then each level's boxes with one root-to-leaf path bold.*

![The first leaves' clusters as runs of the triangle and vertex arrays](../../images/render/baked-mesh-ranges.png)

*A cluster owns a run of each array and indexes only its own vertices.*

A vertex carries one sRGB colour, baked light times albedo. With
`shading = { flat = ... }` each triangle instead has one RGB565 colour
(`light.face_colours()`) averaged over the points `shading.flat` sets, and
vertices weld by position alone.

## Meshlets

Clusters are meshoptimizer's **meshlets**<sup>[[11]](../../Citations.md#meshoptimizer)</sup>:
at most `geometry.meshlet_triangles` (4 to 256, default 32) of one sidedness.
The octree is built over their centres.

![The clusterizer cutting a small flat patch into meshlets, one per step](../../images/render/meshlet-build.gif)

*The importer's clusterizer call on a flat patch, limit 16: whole meshlets in
emitted order, since meshoptimizer has no per-triangle hook.*

![Clusters painted by the meshlets debug view](../../images/render/sponza-meshlets.png)

![Clusters and submitted triangles against the meshlet size](../../images/render/meshlets/meshlet-size-trade.png)

*Measured rows: the [size table](../Render-Pipeline.md#meshlets).*

Meshlets share vertices, so a frame transforms fewer than with octree-cut
leaves. The draw order they set decides depth ties, a fraction of a percent of
pixels. Levels of detail on them: [plans/Cluster-LOD.md](../../plans/Cluster-LOD.md).
