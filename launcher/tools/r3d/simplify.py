"""Collapse lit geometry under shared shape error, preserving contacts and placed copies."""

from copy import copy
from dataclasses import dataclass, replace
import math
import pathlib
import sys

import numpy as np

from . import log
from .geometry import grouped_indices, triangle_areas, weld_keeping
from .import_settings import match_meshes
from .lit_mesh import POSITION_SCALE
from .meshopt import PERMISSIVE, PRUNE, REGULARIZE, REGULARIZE_LIGHT, UNLIMITED_ERROR, simplify_with_update
from .objects import allocate
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
    side = np.sqrt(triangle_areas(pos, tris).sum() / max(budget, 1))
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
    # With seal_seams, where the probes that look for sky through walls stand: (camera region or None, near plane),
    # then optionally the camera path's views, as leaks.LeakProbes takes them.
    camera: object = None
    source: tuple | None = None
    fixed: int = 0
    double_sided: tuple = ()
    held: object = None


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


def simplify(pos, rgb, tris, labels, triangles, *, colour_deviation, seal_seams=False,
             position_scale=POSITION_SCALE, allocation):
    """Collapse the rest together, and carve repeated shapes through their own stand-ins."""
    tri_o = np.zeros(len(tris), dtype=np.int64) if allocation.tri_o is None else np.asarray(allocation.tri_o)
    held = np.zeros(len(tris), dtype=bool) if allocation.held is None else np.asarray(allocation.held, dtype=bool)
    options = REGULARIZE | PERMISSIVE
    if seal_seams:
        pos, rgb, tris, ownership = repair(pos, rgb, tris, np.column_stack([labels, tri_o, held]),
                                           seal_tolerance(position_scale))
        labels, tri_o, held = ownership.T
        held = held.astype(bool)
        options = REGULARIZE_LIGHT | PERMISSIVE
    labels = np.asarray(labels)
    # Held triangles (thinned pieces) pass the collapse whole: their corners are locked, so the rest pays the budget.
    locks = np.zeros(len(pos), dtype=bool)
    locks[tris[held]] = True
    probes = None
    if seal_seams and allocation.camera is not None:
        from .leaks import LeakProbes

        probes = LeakProbes(pos, tris, *allocation.camera)
    return _simplify_meshes(pos, rgb, tris, labels, tri_o, triangles, allocation, colour_deviation,
                            options, position_scale, locks, held, probes)


def _run_pass(pos, rgb, tris, labels, budget, deviation, options, locks=None, leaks=None, record=None, *,
              target_error=UNLIMITED_ERROR):
    """One meshoptimizer pass over `tris`. `leaks` is (the scene's LeakProbes, this pass's rows of its triangles):
    while a view shows sky through the result, the pass runs again with one more source vertex locked per hole.
    A round whose locks leave the pass more triangles than its budget, or than its first run when that is
    already over, is tried again with the half that answers the widest holes, and ends the pass when not one lock
    fits. `record`, a dict, takes the rounds run, the vertices locked and
    the holes the result still shows."""
    used, local = np.unique(tris, return_inverse=True)
    local = local.reshape(-1, 3)
    weight = colour_weight(pos[used], local, budget, deviation)
    held = None if locks is None else locks[used].copy()

    def run():
        kwargs = {'target_error': target_error}
        if held is not None:
            kwargs['vertex_lock'] = held
        points, colours = pos[used], rgb[used]
        result = simplify_with_update(points, colours, local, budget, weight, options, **kwargs)
        # PRUNE can remove an entire component even when some of its vertices are locked.
        if options & PRUNE and held is not None and np.any(held & ~np.isin(np.arange(len(used)), result[3])):
            result = simplify_with_update(points, colours, local, budget, weight, options & ~PRUNE, **kwargs)
        log(f"  simplified {len(tris)} -> {len(result[2])} triangles (budget {budget}, colour weight {weight:.3g})")
        return result

    p, c, t, kept = run()
    if leaks is not None:
        from .leaks import ROUNDS

        probes, rows = leaks
        most = max(len(t), budget)
        rounds = locked = 0

        def look():
            free = np.zeros(len(pos), dtype=bool)
            free[used] = True if held is None else ~held
            return probes.repair(rows, p, t, free)

        holes, pinned = look()
        while len(pinned) and rounds < ROUNDS:
            log(f"  {holes} holes seen, {len(pinned)} vertices to lock")
            before = np.zeros(len(used), dtype=bool) if held is None else held
            while len(pinned):
                held = before.copy()
                held[np.searchsorted(used, pinned)] = True
                again = run()
                if len(again[2]) <= most:
                    break
                log(f"  {len(pinned)} locks leave {len(again[2])} triangles, over the pass's {most}: trying half")
                pinned = pinned[:len(pinned) // 2]
            if not len(pinned):
                held = before
                break
            p, c, t, kept = again
            rounds, locked = rounds + 1, locked + len(pinned)
            holes, pinned = look()
        log(f"  {holes} holes left after {rounds} rounds, {locked} vertices locked")
        probes.finish(rows, p, t)
        if record is not None:
            record.update(leak_rounds=rounds, leak_locks=locked, leak_holes=holes)
    return p, c, t, _label_after(local, labels, kept, t)


def _mesh_passes(tri_o, selected, edges):
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    rest = ~np.isin(tri_o, list(selected))
    parts = [rest] if np.any(rest) else []
    parts.extend(tri_o == obj for obj in selected)
    pass_ids = {obj: [i for i, sel in enumerate(parts) if np.any(sel & (tri_o == obj))]
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
        merged.append(np.logical_or.reduce(members))
    return merged, policies


def _carve_shape(shape, pos, rgb, tris, labels, selected, policies, parts, single, locks,
                 deviation, options, budget, error):
    from scipy.spatial import cKDTree
    from .objects.instances import place
    from .objects.surface_match import JoinedInstance

    stands = []
    positions, triangles = shape.positions, shape.triangles
    union = np.zeros(len(positions), dtype=bool)
    first_indices = None
    placements = []
    for instance in shape.instances:
        obj = int(instance.object_name)
        sel = parts[single[obj]]
        used = np.unique(tris[sel])
        placed = place(positions, instance.linear, instance.position)
        _, nearest = cKDTree(pos[used]).query(placed)
        # A copy joined by surface holds no locked vertex (rule_shapes), so its nearest rows add nothing here.
        indices = used[nearest]
        union |= locks[indices]
        if first_indices is None:
            first_indices = indices
        placements.append((instance, indices))
    stand_in, candidates, budget, moved = stand_in_within(
        positions, rgb[first_indices], triangles, shape.materials, budget, deviation, options,
        union if np.any(union) else None, error)
    p, c, t, kept = stand_in.positions, stand_in.rgb, stand_in.tris, stand_in.kept
    row = dict(objects=[int(i.object_name) for i in shape.instances], copies=len(shape.instances),
                       joined=sum(isinstance(i, JoinedInstance) for i in shape.instances),
                       budget=budget, policy=('lock' if np.any(union) else policies[int(shape.instances[0].object_name)]),
                       locked_vertices=int(union.sum()), output_triangles=len(t),
                       stand_in=stand_in.kind, stand_ins=candidates,
                       locked_border_excess=max(0, len(t) - budget) if np.any(union) else 0,
                       surface_error=moved)
    mat = stand_in.materials
    copies = []
    for instance, indices in placements:
        placed = place(p, instance.linear, instance.position)
        # World-space locked positions must survive local-frame float conversion.
        if kept is not None:
            placed[union[kept]] = pos[indices[kept[union[kept]]]].astype(np.float32)
        faces = t[:, ::-1] if instance.mirrored else t
        stands.append((np.flatnonzero(parts[single[int(instance.object_name)]]), placed, faces))
        own = mat
        if isinstance(instance, JoinedInstance):
            sel = parts[single[int(instance.object_name)]]
            own = own_materials(placed, faces, mat, pos, tris[sel], labels[sel])
        copies.append((instance, (placed, c.copy(), faces, own.copy())))
    carved = dict(positions=p, tris=t, materials=mat, rule=selected[int(shape.instances[0].object_name)],
                  copies=copies)
    return carved, row, stands


def _carved_shapes(pos, tris, labels, tri_o, selected, locks, single, parts):
    """Recover the prototypes that can leave the rest without splitting a welded seam."""
    from .objects.instances import source_objects
    from .objects.carve import rule_groups

    mask = np.isin(tri_o, list(single))
    names = tuple(str(i) for i in range(int(tri_o.max()) + 1))
    objects = {int(obj.name): obj for obj in source_objects(pos, tris[mask], labels[mask], tri_o[mask], names)}
    held = {obj for obj, index in single.items() if np.any(locks[tris[parts[index]]])}
    for members in rule_groups(selected).values():
        candidates = [objects[obj] for obj in members if obj in single]
        if candidates:
            for shape in rule_shapes(candidates, selected[int(candidates[0].name)], held):
                positions, triangles = weld_keeping(shape.positions, shape.triangles)
                yield replace(shape, positions=positions, triangles=triangles)


def _shared_passes(pos, rgb, tris, labels, selected, policies, parts, locks, deviation, options,
                   bundle, prototypes, single, budget, double_sided, probes):
    from .objects.budget import prototype_groups, stored_count

    shapes, shared = [], []
    for shape, piece in prototypes:
        error = allocate.piece_error(piece, budget.error)
        carved, row, stands = _carve_shape(
            shape, pos, rgb, tris, labels, selected, policies, parts, single, locks, deviation, options,
            budget.counts[piece.name], error)
        drawn = len(shape.instances) * row['output_triangles']
        if bundle:
            groups = prototype_groups(carved, double_sided)
            stored, meshes = stored_count(groups), len(groups)
        else:
            stored, meshes = drawn, len(shape.instances)
        row.update(error=error, floor=piece.floor, drawn_triangles=drawn, stored_triangles=stored, stored_meshes=meshes)
        shapes.append(carved)
        shared.append(row)
        if probes is not None:
            for stand in stands:
                probes.finish(*stand)
    return shapes, shared


MIN_BUDGET_GROWTH = 1.25
ALLOCATION_TRIALS = 3


def stand_in_within(positions, rgb, tris, materials, budget, deviation, options, lock, error):
    """best_stand_in() at `budget`, raised until the stand-in's measured surface move is within `error` (model
    units; None bounds nothing) or it keeps every triangle: the carve keeps colour as well as shape, so at the
    count a shape-only simplifier needs for `error` it can move further. Returns (stand_in, candidates, budget,
    move). Surfaces move about as the square root of their triangles, so each try asks (move / error)^2 more."""
    from .objects.detail import surface_error
    from .objects.stand_ins import best_stand_in

    while True:
        stand_in, candidates = best_stand_in(positions, rgb, tris, materials, budget,
                                             colour_weight(positions, tris, budget, deviation), options, lock)
        moved = surface_error(positions, tris, stand_in.positions, stand_in.tris)
        if error is None or moved <= error or budget >= len(tris):
            return stand_in, candidates, budget, moved
        budget = (len(tris) if error == 0 else
                  min(len(tris), math.ceil(max(budget, 1) * max(MIN_BUDGET_GROWTH, (moved / error) ** 2))))


def rule_shapes(candidates, rule, held):
    """A rule's objects as shapes: exact matches, joined by surface when the rule's `shape` is "surface". A copy
    joined by surface does not share the shape's vertices, so it cannot keep a locked border: a shape with a copy
    in `held` (object ids with a locked border) stays exact."""
    from .objects.instances import match_instances
    from .objects.surface_match import merge

    shapes = match_instances(candidates)
    if rule.get('shape', 'exact') == 'exact':
        return shapes
    locked = [any(int(i.object_name) in held for i in shape.instances) for shape in shapes]
    return merge([s for s, h in zip(shapes, locked) if not h]) + [s for s, h in zip(shapes, locked) if h]


def own_materials(placed, faces, materials, pos, own_tris, own_labels):
    """A copy joined by surface keeps its own materials: each shape material becomes the copy material that lies
    nearest to most of that material's stamped triangles."""
    from .geometry import majority, nearest_triangles, triangle_search

    nearest = nearest_triangles(placed[faces].mean(axis=1), triangle_search(pos[own_tris]))[0]
    found = np.asarray(own_labels)[nearest]
    return majority(materials, found, int(materials.max()) + 1)[materials].astype(np.asarray(materials).dtype)


def _lock_contacts(locks, edges, policies):
    for edge in edges:
        if any(policies.get(obj) == 'lock' or (edge.unshared and policies.get(obj) == 'weld')
               for obj in (edge.a, edge.b)):
            locks[edge.vertices_a] = True
            locks[edge.vertices_b] = True
        # A free mesh frees only its own side, so the rest it touches keeps its border.
        for obj, other, vertices in ((edge.a, edge.b, edge.vertices_b), (edge.b, edge.a, edge.vertices_a)):
            if policies.get(obj) == 'free' and other not in policies:
                locks[vertices] = True


def _rest_pass(pos, rgb, tris, labels, rows_by_object, capped, edges, locks, held, budget, deviation, options,
               names, probes):
    """Emit the pooled rest and each object whose own cap is below the shared error."""
    rest_locks = locks.copy()
    _lock_contacts(rest_locks, edges, dict.fromkeys(capped, 'lock'))
    pooled = [obj for obj in rows_by_object if obj not in capped]
    passes = [(pooled, budget.error)] if pooled else []
    passes.extend(([obj], floor) for obj, floor in capped.items())
    emitted, report = [], []
    for objects, error in passes:
        rows = np.sort(np.concatenate([rows_by_object[obj] for obj in objects]))
        target = int(held[rows].sum()) + sum(budget.counts.get(str(obj), 0) for obj in objects)
        leaks = {}
        result = _run_pass(pos, rgb, tris[rows], labels[rows], target, deviation, options | allocate.REST_OPTIONS,
                           rest_locks if np.any(rest_locks) else None, None if probes is None else (probes, rows),
                           leaks, target_error=error)
        report.append(dict(objects=[names[obj] for obj in objects], copies=1, budget=target,
                           error=error, output_triangles=len(result[2]),
                           locked_vertices=int(rest_locks[np.unique(tris[rows])].sum()), **leaks))
        emitted.append(result)
    return emitted, report


def _simplify_meshes(pos, rgb, tris, labels, tri_o, triangles, allocation, deviation, options, position_scale,
                     held_locks, held, probes=None):
    from .objects.contact import contact_graph
    from .objects.instances import source_objects

    selected = (match_meshes(tri_o, labels, allocation.object_names, allocation.material_names, allocation.meshes)
                if allocation.selection is None else {obj: rule for obj, rule in allocation.selection.items()
                                                       if np.any(tri_o == obj)})
    edges = contact_graph(pos, tris, tri_o, position_scale) if selected else ()
    parts, policies = _mesh_passes(tri_o, selected, edges)
    locks = held_locks.copy()
    _lock_contacts(locks, edges, policies)
    single = {}
    for index, sel in enumerate(parts):
        objects = np.unique(tri_o[sel])
        if len(objects) == 1 and objects[0] in selected:
            single[int(objects[0])] = index
    prototypes = [(shape, allocate.Piece(shape.instances[0].object_name, shape.positions, shape.triangles,
                                             len(shape.instances), True))
                  for shape in _carved_shapes(pos, tris, labels, tri_o, selected, locks, single, parts)]
    waiting = [sel for index, sel in enumerate(parts) if index not in single.values()]
    rest = np.logical_or.reduce(waiting) if waiting else np.zeros(len(tris), dtype=bool)

    source = (pos, tris[~held], labels[~held], tri_o[~held]) if allocation.source is None else allocation.source
    source_points, source_faces, source_labels, source_owners = source
    not_carved = ~np.isin(source_owners, list(single))
    object_names = allocation.object_names or tuple(map(str, range(int(tri_o.max(initial=0)) + 1)))
    names = tuple(map(str, range(len(object_names))))
    original_objects = source_objects(source_points, source_faces[not_carved], source_labels[not_carved],
                                      source_owners[not_carved], names)
    rest_pieces = [allocate.Piece(obj.name, *weld_keeping(obj.positions - obj.positions[0], obj.triangles),
                                      1, False)
                   for obj in original_objects]
    pieces = [*(piece for _, piece in prototypes), *rest_pieces]
    rest_indices = np.flatnonzero(rest)
    rest_rows = {obj: rest_indices[rows] for obj, rows in grouped_indices(tri_o[rest]).items()}
    rest_edges = [edge for edge in edges if edge.a in rest_rows and edge.b in rest_rows] if selected else None
    held_count = int(np.count_nonzero(held & rest))
    fixed = allocation.fixed + held_count
    costs = {piece.name: (0, len(shape.instances) if allocation.shapes is None else
                          len({instance.mirrored for instance in shape.instances}))
             for shape, piece in prototypes}
    rest_excess = 0
    for trial in range(ALLOCATION_TRIALS):
        budget = allocate.allocate(pieces, triangles, allocation.ceiling, fixed + rest_excess, costs)
        capped = {int(piece.name): piece.floor for piece in rest_pieces
                  if piece.floor < budget.error and int(piece.name) in rest_rows}
        if capped and rest_edges is None:
            rest_edges = contact_graph(pos, tris[rest], tri_o[rest], position_scale)
        if probes is None:
            trial_probes = None
        else:
            trial_probes = copy(probes)
            trial_probes.source = probes.source.copy()
            trial_probes.finished = []
        shapes, shared = _shared_passes(pos, rgb, tris, labels, selected, policies, parts, locks, deviation,
                                        options, allocation.shapes is not None, prototypes, single, budget,
                                        allocation.double_sided, trial_probes)
        emitted, rest_report = _rest_pass(pos, rgb, tris, labels, rest_rows, capped, rest_edges or (), locks, held,
                                          budget, deviation, options, object_names, trial_probes)
        rest_count = sum(row['output_triangles'] for row in rest_report)
        rest_budget = sum(row['budget'] for row in rest_report)
        drawn = allocation.fixed + rest_count + sum(row['drawn_triangles'] for row in shared)
        stored = allocation.fixed + rest_count + sum(row['stored_triangles'] for row in shared)
        log(f"  shared error trial {trial + 1}: {budget.error:.6g}, drawn {drawn}/{triangles}, "
            f"stored {stored}/{allocation.ceiling}")
        if allocate.within(drawn, stored, triangles, allocation.ceiling):
            break
        rest_excess = max(0, rest_count - rest_budget)
        for (_, piece), row in zip(prototypes, shared):
            count = budget.counts[piece.name]
            costs[piece.name] = (max(0, row['output_triangles'] - count), row['stored_meshes'])
    allocate.check(drawn, stored, triangles, allocation.ceiling)

    shade = allocation.shade_shared or allocation.shade
    for shape in shapes:
        if shade is not None:
            # Shared copies shade in prototype material order, which their triangle list shares.
            shape['copies'] = [(instance, shade(*lit, groups=shape['materials'])) for instance, lit in shape['copies']]
    if emitted:
        result = _join_passes(emitted)
        if len(result[2]) and allocation.shade is not None and np.any(np.isin(tri_o[rest], list(selected))):
            result = allocation.shade(*result)
        emitted = [result]
    if allocation.shapes is None:
        emitted[:0] = [lit for shape in shapes for _, lit in shape['copies']]
    else:
        allocation.shapes.extend(shapes)
    log(f"  allocation_trials={trial + 1}")
    for entry in shared:
        entry['objects'] = [object_names[obj] for obj in entry['objects']]
    if allocation.report is not None:
        allocation.report.update(
            error=budget.error, allocation_trials=trial + 1, rest_budget=rest_budget, ceiling=allocation.ceiling,
            passes=shared + rest_report, meshes=shared, scene_total=drawn, stored_total=stored, output_triangles=drawn,
            contacts=[dict(a=object_names[e.a], b=object_names[e.b], unshared=e.unshared,
                           vertices_a=e.vertices_a.tolist(), vertices_b=e.vertices_b.tolist()) for e in edges])
    if not emitted:
        return pos[:0], rgb[:0], tris[:0], labels[:0]
    positions, colours, triangles, materials = _join_passes(emitted)
    if not selected and options & REGULARIZE_LIGHT:
        rounded = np.clip(np.rint(colours), 0, 255)
        colours = merge_close_colours(np.round(positions * position_scale).astype(np.int64), rounded,
                                      SEAM_COLOUR_TOLERANCE)
    return positions, colours, triangles, materials


def _join_passes(passes):
    """Combine pass-local meshes for scene shading and output."""
    if len(passes) == 1:
        return passes[0]
    out_p, out_c, out_t, out_m = [], [], [], []
    base = 0
    for p, c, t, m in passes:
        out_p.append(p)
        out_c.append(c)
        out_t.append(t + base)
        out_m.append(m)
        base += len(p)
    return np.concatenate(out_p), np.concatenate(out_c), np.concatenate(out_t), np.concatenate(out_m)
