"""Bakes a lit mesh into the r3d_lit_mesh_t C data render/r3d_lit_mesh.h reads,
and reads such data back.

The triangles are cut into meshlets, compact clusters of a few dozen
triangles, under an octree over the meshlets' centres. Positions are
quantized to int16 ticks and the result is checked against the format's
invariants before a byte is written. A mesh's triangles are put in a canonical
order first, so the same triangles always bake to the same bytes. With levels
of detail (lod/bake_levels.py) the meshlets are clusterlod's finest clusters,
and the coarser levels' clusters and group records come with them."""

import pathlib
import sys
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "device"))
import gfx_color  # noqa: E402  (path must be set up first)
from build.c_header import read_define  # noqa: E402
from r3d.mesh_asset import BLOB_HEADER, CLUSTER, LOD_NONE, NODE, TYPE  # noqa: E402
from r3d.geometry import distinct_corners
from r3d.process_budget import NULL_RECORDER
from r3d.meshopt import build_meshlets  # noqa: E402
from r3d.import_settings import MESHLET_TRIANGLES
from r3d.octree import build_octree, flatten_octree, node_bounds  # noqa: E402

INT16_MAX = 32767
MAX_VERTICES = 65535  # uint16 indices
MAX_NODES = 65535
MAX_TRIANGLES = 65535  # uint16 triangle_first
MAX_CLUSTERS = 65535  # uint16 leaf first cluster
MAX_NODE_CHILDREN = 255
# Children of a node that is not a leaf, as the cull walk sorts them.
NODE_FANOUT = read_define(pathlib.Path(__file__).resolve().parents[2] / "main/render/r3d_lit_mesh.h",
                          "R3D_LIT_NODE_CHILDREN_MAX")

LEAF_TRIANGLES = 320
MAX_DEPTH = 10
POSITION_SCALE = 8

# A bake welds positions closer than half an output tick (position_scale ticks per unit) into one vertex.
WELD_PER_TICK = 2


def weld_quantised(q, rgb, tris, double, face=None):
    """(q, rgb, tris, double, face): one vertex per position and colour, in
    order of position then colour, the triangles that collapsed dropped and the
    vertices nothing uses gone. A triangle collapses when two of its corners
    weld, or when it welds onto another one's vertices in the same winding and
    sidedness: the first of those stays. A seam, two vertices at one position
    with different colours, stays a seam. Without vertex colours (`rgb` None, a
    flat mesh) only the position counts; `face`, a colour per triangle, keeps
    the rows of the triangles that stayed."""
    key = q if rgb is None else np.concatenate([q, rgb], axis=1)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    t = inverse.reshape(-1)[tris]
    double = np.asarray(double)
    keep = np.flatnonzero((t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2]))
    _, once = np.unique(np.column_stack([turned(t[keep]), double[keep]]), axis=0, return_index=True)
    keep = keep[np.sort(once)]
    t, double = t[keep], double[keep]
    used, local = np.unique(t, return_inverse=True)
    return (q[first][used], None if rgb is None else rgb[first][used], local.reshape(-1, 3), double,
            None if face is None else np.asarray(face)[keep])


def turned(tris):
    """Each triangle turned to start at its smallest vertex, which keeps its winding."""
    start = np.argmin(tris, axis=1)
    rows = np.arange(len(tris))
    return np.stack([tris[rows, (start + k) % 3] for k in range(3)], axis=1).reshape(-1, 3)


def canonical_order(tris, double, face=None):
    """(tris, double, face): the triangles each turned to start at its
    smallest vertex, which keeps its winding, and sorted, so the order says
    nothing of where they came from. `face` follows its triangles."""
    tris = turned(tris)
    order = np.lexsort((tris[:, 2], tris[:, 1], tris[:, 0]))
    return tris[order], np.asarray(double)[order], None if face is None else np.asarray(face)[order]


def oriented(t):
    """A triangle's vertices turned to start at the smallest, which keeps the
    winding: the same key for a triangle however the meshlet builder turned it,
    and a different one for its back-to-back twin."""
    k = int(np.argmin(t))
    return (int(t[k]), int(t[(k + 1) % 3]), int(t[(k + 2) % 3]))


def local_vertices(tris):
    """(vertices, local tris): the vertices a triangle list uses in order of
    first use, and the triangles indexing that list."""
    unique, first, inverse = np.unique(tris.reshape(-1), return_index=True, return_inverse=True)
    order = np.argsort(first)
    rank = np.empty(len(unique), dtype=np.int64)
    rank[order] = np.arange(len(unique))
    return unique[order], rank[inverse].reshape(-1, 3)


def bake_lit_mesh(positions, rgb, tris, double, position_scale=POSITION_SCALE, leaf_triangles=LEAF_TRIANGLES,
                  max_depth=MAX_DEPTH, meshlet_triangles=MESHLET_TRIANGLES, face_rgb=None):
    """Bakes positions (model units), rgb (0..255 per vertex) and tris
    (counter-clockwise seen from the front, `double` one flag per triangle)
    into a SimpleNamespace holding pos, rgb, tris, clusters, nodes, the
    position_scale and face_colors. A cluster holds at most meshlet_triangles
    triangles of one sidedness; an octree leaf holds meshlets to leaf_triangles
    triangles. Given face_rgb, one 0..255 colour per triangle, the mesh is
    flat: rgb is None, vertices weld by position alone and face_colors holds
    the colours; otherwise face_colors is None. With neither input colour
    array, the mesh is colourless."""
    q = np.round(np.asarray(positions) * position_scale).astype(np.int64)
    if not len(tris):
        anchor = q[:1] if len(q) else np.zeros((1, 3), dtype=np.int64)
        lo = hi = anchor[0]
        return SimpleNamespace(pos=anchor, rgb=np.full((1, 3), np.iinfo(np.uint8).max), tris=np.empty((0, 3), dtype=np.int64),
                               clusters=[(0, 1, 0, 0, lo, hi, False)],
                               nodes=[dict(lo=lo, hi=hi, first=0, count=1, leaf=True)],
                               position_scale=position_scale, face_colors=None)
    flat = face_rgb is not None
    col = None if flat or rgb is None else np.clip(np.rint(rgb), 0, 255).astype(np.int64)
    face = np.clip(np.rint(face_rgb), 0, 255).astype(np.int64) if flat else None
    assert not flat or len(face) == len(tris), "a flat bake needs one face colour per triangle"
    q, col, tris, double, face = weld_quantised(q, col, np.asarray(tris, dtype=np.int64), double, face)
    tris, double, face = canonical_order(tris, double, face)
    return assemble(q, col, meshlet_entries(q, tris, double, face, meshlet_triangles), position_scale,
                    leaf_triangles, max_depth)


def meshlet_entries(q, tris, double, face, meshlet_triangles):
    """The clusters of welded triangles in canonical order: meshlets of one
    sidedness each, as dicts of "double", "tris" (indices into q) and "face"
    (their colours, None unless flat)."""
    entries = []
    for is_double in (0, 1):
        member = double == is_double
        members = tris[member]
        if len(members):
            colours = {oriented(t): c for t, c in zip(members, face[member])} if face is not None else None
            assert colours is None or len(colours) == len(members), "two triangles repeat one winding of the same vertices"
            entries += [{"double": is_double, "tris": t,
                         "face": None if colours is None else np.array([colours[oriented(x)] for x in t])}
                        for t in build_meshlets(q, members, meshlet_triangles)]
    return entries


def assemble(q, col, entries, position_scale, leaf_triangles, max_depth, groups=None):
    """The baked mesh of clusters cut by any means: an octree over their
    centres, each cluster owning its vertices, checked against the format.
    With `groups`, each entry's "own" and "parent" become the cluster's
    group pair."""
    for e in entries:
        e["box"] = box_of(q, e["tris"])
    centres = np.array([(lo + hi) / 2.0 for lo, hi in (e["box"] for e in entries)])
    weights = np.array([len(e["tris"]) for e in entries])

    def tree(items):
        order, nodes = flatten_octree(build_octree(centres[items], weights[items], leaf_triangles, max_depth))
        return [int(items[i]) for i in order], nodes

    coarse = np.array([groups is not None and e["own"] != LOD_NONE for e in entries], dtype=bool)
    if coarse.any():
        by_own = sorted(np.flatnonzero(coarse), key=lambda i: entries[i]["own"])
        order, nodes = two_trees(*tree(np.flatnonzero(~coarse)), *run_tree(by_own, weights, leaf_triangles))
    else:
        order, nodes = tree(np.arange(len(entries)))

    out = SimpleNamespace(position_scale=position_scale)
    ordered = [entries[i] for i in order]
    out.pos, out.rgb, out.tris, out.clusters, out.face_colors = lay_out(q, col, ordered)
    node_bounds(nodes, out.clusters)
    out.nodes = nodes
    out.lod = None if groups is None else [(e["own"], e["parent"]) for e in ordered]
    out.groups = None if groups is None else np.asarray(groups, dtype=np.int64).reshape(-1, 5)
    validate(out.pos, out.rgb, out.tris, out.clusters, out.nodes, out.face_colors)
    validate_levels(out.lod, out.groups, len(out.clusters))
    return out


def run_tree(items, weights, leaf_weight):
    """(order, nodes): `items` kept in their order, under leaves of
    consecutive items up to `leaf_weight` (and MAX_NODE_CHILDREN items), and
    those under nodes of up to NODE_FANOUT children, breadth first."""
    leaves, start, weight = [], 0, 0
    for i, item in enumerate(items):
        if i > start and (weight + weights[item] > leaf_weight or i - start == MAX_NODE_CHILDREN):
            leaves.append({"leaf": True, "first": start, "count": i - start})
            start, weight = i, 0
        weight += weights[item]
    leaves.append({"leaf": True, "first": start, "count": len(items) - start})
    level = leaves
    while len(level) > 1:
        level = [{"leaf": False, "children": level[i:i + NODE_FANOUT]} for i in range(0, len(level), NODE_FANOUT)]
    queue, nodes = [level[0]], []
    for node in queue:
        if node["leaf"]:
            nodes.append(node)
        else:
            nodes.append({"leaf": False, "first": len(queue), "count": len(node["children"])})
            queue.extend(node["children"])
    return [int(i) for i in items], nodes


def two_trees(finest_order, finest_nodes, coarse_order, coarse_nodes):
    """One tree whose root's first child is the finest clusters' tree and its
    second the coarse clusters', in the order of the group simplified into
    each: the cull walks the first, then takes the coarse clusters of the
    picked cut straight from their groups (r3d_lit_mesh_finest_root()). Each
    tree keeps its own node order after the two roots, and its clusters stay
    one run."""
    finest_count = len(finest_nodes)

    def placed(node, nodes_at, items_at):
        if node["leaf"]:
            return dict(node, first=node["first"] + items_at)
        return dict(node, first=nodes_at(node["first"]))

    def finest_at(j):
        return 1 if j == 0 else 2 + j

    def coarse_at(k):
        return 2 if k == 0 else finest_count + 1 + k

    root = {"leaf": False, "first": 1, "count": 2}
    finest = [placed(n, finest_at, 0) for n in finest_nodes]
    coarse = [placed(n, coarse_at, len(finest_order)) for n in coarse_nodes]
    return finest_order + coarse_order, [root, finest[0], coarse[0], *finest[1:], *coarse[1:]]


def box_of(q, tris):
    p = q[np.unique(tris.reshape(-1))]
    return p.min(axis=0), p.max(axis=0)


def lay_out(q, col, entries):
    """The vertex, colour and triangle arrays of a run of clusters, each
    owning the vertices its triangles use, and the cluster rows over them.
    Absent colour arrays stay absent."""
    flat = entries[0]["face"] is not None
    pos, rgb, tris, clusters, face = [], [], [], [], []
    vbase = tbase = 0
    for e in entries:
        used, local = local_vertices(e["tris"])
        pos.append(q[used])
        tris.append(local + vbase)
        if flat:
            face.append(e["face"])
        elif col is not None:
            rgb.append(col[used])
        clusters.append((vbase, len(used), tbase, len(local), e["box"][0], e["box"][1], bool(e["double"])))
        vbase += len(used)
        tbase += len(local)
    return (np.concatenate(pos), np.concatenate(rgb) if rgb else None, np.concatenate(tris), clusters,
            np.concatenate(face) if flat else None)


def validate(pos, rgb, tris, clusters, nodes, face_colors=None):
    assert len(pos) <= MAX_VERTICES, f"{len(pos)} vertices exceed uint16 indices"
    assert len(tris) <= MAX_TRIANGLES, f"{len(tris)} triangles exceed uint16 offsets"
    assert len(clusters) <= MAX_CLUSTERS, f"{len(clusters)} clusters exceed uint16 offsets"
    assert np.abs(pos).max() <= INT16_MAX, "a position does not fit int16"
    assert face_colors is None or rgb is None, "vertex colours or face colours, never both"
    colours = rgb if rgb is not None else face_colors
    if colours is not None:
        assert colours.min() >= 0 and colours.max() <= 255
        assert len(colours) == (len(pos) if rgb is not None else len(tris))
    assert tris.min() >= 0 and tris.max() < len(pos)
    assert np.all(distinct_corners(tris))
    next_v = next_t = 0
    for vbase, vcount, tbase, tcount, lo, hi, _ in clusters:
        assert vbase == next_v and tbase == next_t, "clusters must tile both arrays in order"
        ct = tris[tbase : tbase + tcount]
        assert ct.min() >= vbase and ct.max() < vbase + vcount, "a triangle reaches outside its cluster"
        cp = pos[vbase : vbase + vcount]
        assert np.all(cp >= lo) and np.all(cp <= hi)
        next_v, next_t = vbase + vcount, tbase + tcount
    assert next_v == len(pos) and next_t == len(tris)
    reached = np.zeros(len(clusters), dtype=np.int64)
    stack = [0]
    while stack:
        node = nodes[stack.pop()]
        assert node["count"] <= MAX_NODE_CHILDREN
        if node["leaf"]:
            reached[node["first"] : node["first"] + node["count"]] += 1
        else:
            stack.extend(range(node["first"], node["first"] + node["count"]))
    assert np.all(reached == 1), "every cluster must hang off exactly one leaf"
    assert len(nodes) <= MAX_NODES


def validate_levels(lod, groups, clusters):
    """Check the level records with their entry-format owner."""
    from r3d import lod_asset
    if lod is None:
        assert groups is None
    else:
        assert len(lod) == clusters
        lod_asset.encode(lod, groups)


def write_lit_mesh(out_dir, name, positions, rgb, tris, double, recorder=None, colourless=False, mesh_builder=bake_lit_mesh, **options):
    """Writes <name>.bundle into out_dir, the pack entry main/render/
    r3d_lit_mesh.h reads. positions are model units, rgb 0..255 per vertex
    (None for a flat mesh), tris counter-clockwise seen from the front,
    double one flag per triangle; options, face_rgb among them, go to
    bake_lit_mesh. With colourless, colour columns are weld keys returned in
    colour_keys or face_keys; neither colour array is written. Returns the baked mesh."""
    recorder = NULL_RECORDER if recorder is None else recorder
    with recorder.step("meshlets", len(tris)) as step:
        mesh = mesh_builder(positions, rgb, tris, double, **options)
        step["triangles_out"] = len(mesh.tris)
    if colourless:
        mesh.colour_keys, mesh.face_keys = mesh.rgb, mesh.face_colors
        mesh.rgb = mesh.face_colors = None
    with recorder.step("write", len(mesh.tris)):
        from r3d import mesh_bundle
        (pathlib.Path(out_dir) / f"{name}{mesh_bundle.SUFFIX}").write_bytes(
            mesh_bundle.encode(mesh_bundle.mesh_entries(name, mesh), []))
    return mesh


def panel_colour(rgb):
    """The panel's RGB565 with its bytes swapped, what RGB565BE(0xRRGGBB) gives."""
    c = np.asarray(rgb, dtype=np.int64)
    return gfx_color.swap(gfx_color.rgb565(c[:, 0], c[:, 1], c[:, 2])).astype("<u2")


def rgb888(panel):
    """The 0..255 channels a panel colour stands for, with the low bits
    refilled from the high ones so panel_colour() returns the same value."""
    p = gfx_color.swap(np.asarray(panel, dtype=np.int64))
    return np.stack(gfx_color.expand(p >> 11, (p >> 5) & 63, p & 31), axis=1)


def mesh_blob(mesh):
    """The entry's bytes for a baked mesh: BLOB_HEADER, then each array at an
    offset from the entry's first byte, in the layouts r3d_lit_mesh.h's
    structs have."""
    arrays = [
        ("positions", np.asarray(mesh.pos, dtype="<i2").tobytes()),
        ("colors", b"" if mesh.rgb is None else np.asarray(mesh.rgb, dtype="u1").tobytes()),
        ("triangles", np.asarray(mesh.tris, dtype="<u2").tobytes()),
        ("clusters", b"".join(CLUSTER.pack(vb, vc, tb, tc, *lo, *hi, int(dbl)) for vb, vc, tb, tc, lo, hi, dbl in mesh.clusters)),
        ("nodes", b"".join(NODE.pack(*n["lo"], *n["hi"], n["first"], n["count"], int(n["leaf"])) for n in mesh.nodes)),
        ("face_colors", b"" if mesh.face_colors is None else panel_colour(mesh.face_colors).tobytes()),
    ]
    offsets, body, at = {}, b"", BLOB_HEADER.size
    for key, data in arrays:
        offsets[key] = at if data else 0
        body += data + bytes(-len(data) % 4)
        at += len(data) + (-len(data) % 4)
    head = BLOB_HEADER.pack(len(mesh.pos), len(mesh.tris), len(mesh.clusters), len(mesh.nodes), mesh.position_scale,
                            *(offsets[key] for key, _ in arrays))
    return head + body


def lod_blob(mesh):
    """The sibling LLOD bytes, or None for a mesh without levels."""
    from r3d import lod_asset
    return None if mesh.lod is None else lod_asset.encode(mesh.lod, mesh.groups)


def read_lit_mesh(path, levels=None):
    """Reads a <name>.bundle back into what bake_lit_mesh returns."""
    if not isinstance(path, (bytes, bytearray)):
        from r3d import mesh_bundle
        entries, copies = mesh_bundle.decode(pathlib.Path(path).read_bytes())
        return mesh_bundle.read_mesh(entries, mesh_bundle.rest_name(entries, copies))
    blob = path
    vertices, triangles, clusters, nodes, scale, *at = BLOB_HEADER.unpack_from(blob)
    pos_at, col_at, tri_at, cl_at, node_at, face_at = at

    def array(offset, dtype, count):
        return np.frombuffer(blob, dtype=dtype, count=count, offset=offset).astype(np.int64)

    mesh = SimpleNamespace(position_scale=scale)
    mesh.pos = array(pos_at, "<i2", vertices * 3).reshape(-1, 3)
    mesh.rgb = array(col_at, "u1", vertices * 3).reshape(-1, 3) if col_at else None
    mesh.face_colors = rgb888(array(face_at, "<u2", triangles)) if face_at else None
    mesh.tris = array(tri_at, "<u2", triangles * 3).reshape(-1, 3)
    mesh.clusters = []
    for i in range(clusters):
        vb, vc, tb, tc, *box, dbl = CLUSTER.unpack_from(blob, cl_at + i * CLUSTER.size)
        mesh.clusters.append((vb, vc, tb, tc, np.array(box[:3]), np.array(box[3:]), bool(dbl)))
    mesh.nodes = []
    for i in range(nodes):
        *box, first, count, leaf = NODE.unpack_from(blob, node_at + i * NODE.size)
        mesh.nodes.append({"lo": np.array(box[:3]), "hi": np.array(box[3:]), "first": first, "count": count, "leaf": bool(leaf)})
    mesh.lod = mesh.groups = None
    if levels is not None:
        from r3d import lod_asset
        lod_asset.validate_mesh(levels, blob)
        mesh.lod, groups = lod_asset.decode(levels)
        mesh.groups = np.asarray(groups, dtype=np.int64)
        validate_levels(mesh.lod, mesh.groups, clusters)
    return mesh


def finest(mesh):
    """Whether each cluster of a read mesh is on its finest level."""
    if mesh.lod is None:
        return np.ones(len(mesh.clusters), dtype=bool)
    return np.array([own == LOD_NONE for own, _ in mesh.lod], dtype=bool)


def finest_triangles(mesh):
    """(pos, rgb, tris, double, face) of a read mesh's finest level with the
    per-cluster vertex copies welded back together: one vertex per position
    and colour, or per position alone for a flat mesh, whose rgb is None."""
    double = np.zeros(len(mesh.tris), dtype=np.int64)
    shown = np.zeros(len(mesh.tris), dtype=bool)
    for (_, _, tbase, tcount, _, _, is_double), level0 in zip(mesh.clusters, finest(mesh)):
        double[tbase : tbase + tcount] = int(is_double)
        shown[tbase : tbase + tcount] = level0
    face = None if mesh.face_colors is None else mesh.face_colors[shown]
    return weld_quantised(mesh.pos, mesh.rgb, mesh.tris[shown], double[shown], face)
