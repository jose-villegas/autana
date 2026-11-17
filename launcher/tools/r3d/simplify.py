"""Appearance-preserving simplification of a lit mesh: split evenly, bake
colour per vertex (the caller does that), then one meshoptimizer pass per
part with colour as an attribute. Groups of materials can hold a reserved
share of the budget, so small detailed props are not starved by large
surfaces a global pass prefers to keep."""

from dataclasses import dataclass
import pathlib
import sys

import numpy as np

from . import log
from .geometry import triangle_areas, weld_keeping
from .import_settings import SettingsError, match_meshes
from .lit_mesh import POSITION_SCALE
from .meshopt import PERMISSIVE, REGULARIZE, REGULARIZE_LIGHT, simplify_with_update
from .repair import repair, seal_tolerance
from .tessellate import split_marked_edges

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "device"))
import gfx_color  # noqa: E402  (path must be set up first)

# A step and a half of RGB565 is what keeps the meshlets sharing their
# vertices on the meshes measured: one step leaves the larger of them with
# more vertices than triangles.
SEAM_COLOUR_TOLERANCE = 1.5 * np.array(gfx_color.STEP)


def densify(p, tris, labels, max_edge, rounds=16):
    """Conforming splits until no edge is longer than max_edge; each piece
    keeps its triangle's label."""
    labels = np.asarray(labels)
    for _ in range(rounds):
        edges = np.sort(np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]]), axis=1)
        # Numeric keys preserve the lexicographic order of non-negative vertex pairs.
        keys = np.unique(edges[:, 0].astype(np.int64) * len(p) + edges[:, 1])
        edges = np.column_stack([keys // len(p), keys % len(p)])
        long = edges[np.linalg.norm(p[edges[:, 0]] - p[edges[:, 1]], axis=1) > max_edge]
        if len(long) == 0:
            break
        p, tris, _, parent = split_marked_edges(p, tris, set(map(tuple, long)))
        labels = labels[parent]
    return p, tris, labels


def colour_weight(pos, tris, budget, deviation):
    """Price one panel colour step against surface deviation, using the side of
    the surface area per budgeted triangle as meshoptimizer's attribute scale."""
    side = np.sqrt(triangle_areas(pos, tris).sum() / budget)
    return deviation / (side * max(gfx_color.STEP)) if side > 0 else 0.0


def _label_after(tris_in, labels_in, kept, tris_out):
    """Each output triangle's label: the label most of its vertices carried
    in the input, read through the vertices the simplifier kept."""
    vertex_label = np.full(int(tris_in.max()) + 1, -1)
    for k in range(3):
        vertex_label[tris_in[:, k]] = labels_in
    corner = vertex_label[kept][tris_out]
    same01 = corner[:, 0] == corner[:, 1]
    return np.where(same01 | (corner[:, 0] == corner[:, 2]), corner[:, 0], corner[:, 1])


def merge_close_colours(q, rgb, tolerance):
    """rgb with each vertex that shares a position (a row of `q`) with an
    earlier vertex, in colour order, whose colour is within `tolerance` on
    every channel given that vertex's colour. What stays differs from every
    other colour at its position by more than the tolerance, so merging
    again changes nothing."""
    _, ids, count = np.unique(q, axis=0, return_inverse=True, return_counts=True)
    order = np.argsort(ids.reshape(-1), kind="stable")
    bounds = np.concatenate([[0], np.cumsum(count)])
    out = rgb.copy()
    for group in np.flatnonzero(count > 1):
        members = order[bounds[group]:bounds[group + 1]]
        kept = []
        for i in members[np.lexsort(rgb[members].T[::-1])]:
            near = next((c for c in kept if np.all(np.abs(rgb[i] - c) <= tolerance)), None)
            if near is None:
                kept.append(rgb[i])
            else:
                out[i] = near
    return out


@dataclass
class Allocation:
    """Source ownership, mesh budgets and post-placement shading."""
    tri_o: object = None
    object_names: tuple = ()
    material_names: tuple = ()
    meshes: tuple = ()
    selection: object = None
    ceiling: object = None
    shade: object = None
    shade_shared: object = None
    report: object = None
    shapes: object = None


def object_groups(pos, tris, tri_o):
    """Objects sharing a welded vertex form one pass, so no pass splits a shared edge."""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    _, welded = weld_keeping(pos, tris)
    owners, inverse = np.unique(tri_o, return_inverse=True)
    corners = np.column_stack([welded.ravel(), np.repeat(inverse, 3)])
    corners = np.unique(corners, axis=0)
    shared = corners[1:, 0] == corners[:-1, 0]
    a, b = corners[:-1, 1][shared], corners[1:, 1][shared]
    graph = coo_matrix((np.ones(len(a)), (a, b)), shape=(len(owners), len(owners)))
    _, groups = connected_components(graph, directed=False)
    return groups[inverse]


def simplify(pos, rgb, tris, labels, triangles, reserved=(), *, colour_deviation, seal_seams=False,
             position_scale=POSITION_SCALE, allocation=None):
    """Material shares budget the rest; carved meshes own independent per-copy budgets."""
    allocation = Allocation() if allocation is None else allocation
    tri_o = np.zeros(len(tris), dtype=np.int64) if allocation.tri_o is None else np.asarray(allocation.tri_o)
    options = REGULARIZE | PERMISSIVE
    if seal_seams:
        pos, rgb, tris, ownership = repair(pos, rgb, tris, np.column_stack([labels, tri_o]), seal_tolerance(position_scale))
        labels, tri_o = ownership.T
        options = REGULARIZE_LIGHT | PERMISSIVE
    labels = np.asarray(labels)
    parts, taken = [], np.zeros(len(tris), dtype=bool)
    if allocation.meshes:
        return _simplify_meshes(pos, rgb, tris, labels, tri_o, triangles, reserved, allocation,
                                colour_deviation, options, position_scale)
    for group, share in reserved:
        sel = np.isin(labels, list(group)) & ~taken
        parts.append((sel, max(1, int(triangles * share))))
        taken |= sel
    parts.append((~taken, max(1, triangles - sum(n for _, n in parts))))
    out_pos, out_rgb, out_tris, out_labels = [], [], [], []
    base = 0
    for sel, budget in parts:
        if budget == 0 or not np.any(sel):
            continue
        p, c, t, m = _run_pass(pos, rgb, tris[sel], labels[sel], budget, colour_deviation, options)
        out_pos.append(p)
        out_rgb.append(c)
        out_tris.append(t + base)
        out_labels.append(m)
        base += len(p)
    out_pos, out_rgb = np.concatenate(out_pos), np.concatenate(out_rgb)
    if seal_seams:
        rounded = np.clip(np.rint(out_rgb), 0, 255)
        out_rgb = merge_close_colours(np.round(out_pos * position_scale).astype(np.int64), rounded,
                                      SEAM_COLOUR_TOLERANCE)
    check_ceiling(sum(len(t) for t in out_tris), allocation.ceiling)
    return out_pos, out_rgb, np.concatenate(out_tris), np.concatenate(out_labels)


def _run_pass(pos, rgb, tris, labels, budget, deviation, options, locks=None):
    used, local = np.unique(tris, return_inverse=True)
    local = local.reshape(-1, 3)
    weight = colour_weight(pos[used], local, budget, deviation)
    kwargs = {} if locks is None else {'vertex_lock': locks[used]}
    p, c, t, kept = simplify_with_update(pos[used], rgb[used], local, budget, weight, options, **kwargs)
    log(f"  simplified {len(tris)} -> {len(t)} triangles (budget {budget}, colour weight {weight:.3g})")
    return p, c, t, _label_after(local, labels, kept, t)


def check_ceiling(total, ceiling):
    """The scene ceiling caps emitted triangles, including every placed copy."""
    if ceiling is not None and total > ceiling:
        raise SettingsError(f"scene emitted triangles {total} exceeds ceiling {ceiling}")
    return total


def _mesh_passes(tris, labels, tri_o, triangles, reserved, selected, edges):
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    rest = ~np.isin(tri_o, list(selected))
    taken = ~rest
    parts = []
    for group, share in reserved:
        sel = np.isin(labels, list(group)) & ~taken
        if np.any(sel):
            parts.append((sel, max(1, int(triangles * share))))
        taken |= sel
    if np.any(~taken):
        parts.append((~taken, max(1, triangles - sum(n for _, n in parts))))
    for obj, rule in selected.items():
        parts.append((tri_o == obj, rule['triangles']))
    pass_ids = {obj: [i for i, (sel, _) in enumerate(parts) if np.any(sel & (tri_o == obj))]
                for obj in np.unique(tri_o)}
    links = []
    touching = {obj for edge in edges for obj in (edge.a, edge.b)}
    policies = {obj: rule.get('seam', 'lock' if obj in touching else 'free') for obj, rule in selected.items()}
    for edge in edges:
        if any(policies.get(obj) == 'weld' for obj in (edge.a, edge.b)):
            links.extend((a, b) for a in pass_ids[edge.a] for b in pass_ids[edge.b])
    a, b = np.array(links, dtype=int).reshape(-1, 2).T
    graph = coo_matrix((np.ones(len(a)), (a, b)), shape=(len(parts), len(parts)))
    _, groups = connected_components(graph, directed=False)
    merged = []
    for group in np.unique(groups):
        members = [parts[i] for i in np.flatnonzero(groups == group)]
        merged.append((np.logical_or.reduce([sel for sel, _ in members]), sum(n for _, n in members)))
    return merged, policies


def _shared_passes(pos, rgb, tris, labels, tri_o, selected, policies, parts, locks, deviation, options, shade, shapes=None):
    from scipy.spatial import cKDTree
    from .objects.instances import match_instances, place, source_objects

    single = {}
    for index, (sel, budget) in enumerate(parts):
        objects = np.unique(tri_o[sel])
        if len(objects) == 1 and objects[0] in selected:
            single[int(objects[0])] = index
    names = tuple(str(i) for i in range(int(tri_o.max()) + 1))
    objects = source_objects(pos, tris, labels, tri_o, names)
    buckets = {}
    for obj in objects:
        obj_id = int(obj.name)
        if obj_id in single:
            rule = selected[obj_id]
            buckets.setdefault(id(rule), []).append(obj)
    emitted, consumed, shared = [], set(), []
    for candidates in buckets.values():
        for shape in match_instances(candidates):
            if len(shape.instances) == 1:
                continue
            union = np.zeros(len(shape.positions), dtype=bool)
            first_indices = None
            placements = []
            for instance in shape.instances:
                obj = int(instance.object_name)
                sel, budget = parts[single[obj]]
                used = np.unique(tris[sel])
                placed = place(shape.positions, instance.linear, instance.position)
                _, nearest = cKDTree(pos[used]).query(placed)
                indices = used[nearest]
                union |= locks[indices]
                if first_indices is None:
                    first_indices = indices
                placements.append((instance, indices))
                consumed.add(single[obj])
            p, c, t, kept = simplify_with_update(shape.positions, rgb[first_indices], shape.triangles, budget,
                                                colour_weight(shape.positions, shape.triangles, budget, deviation),
                                                options, vertex_lock=union if np.any(union) else None)
            shared.append(dict(objects=[int(i.object_name) for i in shape.instances], copies=len(shape.instances),
                               budget=budget, policy=('lock' if np.any(union) else policies[int(shape.instances[0].object_name)]),
                               locked_vertices=int(union.sum()), output_triangles=len(t),
                               locked_border_excess=max(0, len(t) - budget) if np.any(union) else 0))
            mat = _label_after(shape.triangles, shape.materials, kept, t)
            copies = []
            for instance, indices in placements:
                placed = place(p, instance.linear, instance.position)
                # World-space locked positions must survive local-frame float conversion.
                placed[union[kept]] = pos[indices[kept[union[kept]]]].astype(np.float32)
                faces = t[:, ::-1] if instance.mirrored else t
                result = placed, c.copy(), faces, mat.copy()
                lit = shade(*result) if shade is not None else result
                if shapes is None:
                    emitted.append(lit)
                else:
                    copies.append((instance, lit))
            if shapes is not None:
                shapes.append(dict(positions=p, tris=t, materials=mat, rule=selected[int(shape.instances[0].object_name)],
                                   copies=copies))
    return emitted, consumed, shared


def _simplify_meshes(pos, rgb, tris, labels, tri_o, triangles, reserved, allocation, deviation, options, position_scale):
    from .objects.contact import contact_graph
    from .objects.instances import Instance

    selected = (match_meshes(tri_o, labels, allocation.object_names, allocation.material_names, allocation.meshes)
                if allocation.selection is None else {obj: rule for obj, rule in allocation.selection.items() if np.any(tri_o == obj)})
    rest_budget = triangles if np.any(~np.isin(tri_o, list(selected))) else 0
    requested = rest_budget + sum(rule['triangles'] for rule in selected.values())
    edges = contact_graph(pos, tris, tri_o, position_scale)
    parts, policies = _mesh_passes(tris, labels, tri_o, triangles, reserved, selected, edges)
    locks = np.zeros(len(pos), dtype=bool)
    for edge in edges:
        if any(policies.get(obj) == 'lock' or (edge.unshared and policies.get(obj) == 'weld')
               for obj in (edge.a, edge.b)):
            locks[edge.vertices_a] = True
            locks[edge.vertices_b] = True
        # A free mesh frees only its own side: the rest it touches keeps its border, so the walls stay closed.
        for obj, other, vertices in ((edge.a, edge.b, edge.vertices_b), (edge.b, edge.a, edge.vertices_a)):
            if policies.get(obj) == 'free' and other not in selected:
                locks[vertices] = True
    report = dict(requested_scene_total=requested, rest_budget=rest_budget, ceiling=allocation.ceiling, passes=[],
                  contacts=[dict(a=allocation.object_names[e.a], b=allocation.object_names[e.b], unshared=e.unshared,
                                 vertices_a=e.vertices_a.tolist(), vertices_b=e.vertices_b.tolist()) for e in edges],
                  meshes=[dict(object=allocation.object_names[obj], copies=1, budget=rule['triangles'], policy=policies[obj],
                               locked_vertices=int(locks[np.unique(tris[tri_o == obj])].sum())) for obj, rule in selected.items()])
    log(f"  scene requested triangles {requested}, ceiling {allocation.ceiling}")
    emitted, consumed, shared = _shared_passes(pos, rgb, tris, labels, tri_o, selected, policies, parts, locks,
                                       deviation, options, allocation.shade_shared or allocation.shade, allocation.shapes)
    shared_objects = {obj for entry in shared for obj in entry['objects']}
    report['meshes'] = [row for obj, row in zip(selected, report['meshes']) if obj not in shared_objects]
    for entry in shared:
        entry['objects'] = [allocation.object_names[obj] for obj in entry['objects']]
        report['meshes'].append(entry)
        report['passes'].append(entry)
    for index, (sel, budget) in enumerate(parts):
        if index in consumed:
            continue
        result = _run_pass(pos, rgb, tris[sel], labels[sel], budget, deviation, options,
                           locks if np.any(locks) else None)
        objects = np.unique(tri_o[sel])
        carved = allocation.shapes is not None and len(objects) == 1 and objects[0] in selected
        shade = (allocation.shade_shared or allocation.shade) if carved else allocation.shade
        if shade is not None and np.any(np.isin(tri_o[sel], list(selected))):
            result = shade(*result)
        entry = dict(objects=[allocation.object_names[obj] for obj in objects], copies=1, budget=budget,
                     output_triangles=len(result[2]), locked_vertices=int(locks[np.unique(tris[sel])].sum()),
                     locked_border_excess=max(0, len(result[2]) - budget) if np.any(locks[tris[sel]]) else 0)
        report['passes'].append(entry)
        for row in report['meshes']:
            if row.get('object') in entry['objects']:
                row.update(output_triangles=entry['output_triangles'], pass_objects=entry['objects'],
                           pass_budget=budget, locked_border_excess=entry['locked_border_excess'])
        if carved:
            instance = Instance(str(objects[0]), np.eye(3), np.zeros(3), False, 0.0)
            allocation.shapes.append(dict(positions=result[0], tris=result[2], materials=result[3],
                                         rule=selected[objects[0]], copies=[(instance, result)]))
        else:
            emitted.append(result)
    out_p, out_c, out_t, out_m = [], [], [], []
    base = 0
    for p, c, t, m in emitted:
        out_p.append(p)
        out_c.append(c)
        out_t.append(t + base)
        out_m.append(m)
        base += len(p)
    report['scene_total'] = report['output_triangles'] = (sum(len(t) for t in out_t) +
        (sum(len(shape['copies'][0][1][2]) * len(shape['copies']) for shape in allocation.shapes) if allocation.shapes is not None else 0))
    if allocation.report is not None:
        allocation.report.update(report)
    check_ceiling(report['scene_total'], allocation.ceiling)
    if not out_p:
        return pos[:0], rgb[:0], tris[:0], labels[:0]
    return np.concatenate(out_p), np.concatenate(out_c), np.concatenate(out_t), np.concatenate(out_m)
