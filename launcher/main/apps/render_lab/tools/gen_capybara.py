#!/usr/bin/env python3
"""Generate main/apps/render_lab/assets/capybara.glb - a rigged, animated
low-poly capybara, the test asset for skinned-mesh rendering.

    python main/apps/render_lab/tools/gen_capybara.py [--out PATH]

Run from launcher/. GENERATED ASSET, ours: modelled entirely by the code
below, no external mesh or data. Standard library only; the output is a plain
glTF 2.0 binary (one skinned primitive, one skin, animations "idle" and
"walk") so any glTF reader loads it. Metres, +Y up, the animal faces +Z and
its left is +X. The walk is in place: no root motion, the planted feet move
backwards at the treadmill speed the report prints.

The legs of both animations are posed by two-bone IK towards planted or
stepping feet, so the body can breathe and bob without the feet sinking or
sliding. The file is checked (closed outward-wound mesh, 4 normalised
weights per vertex, every influence near its joint, loops that close) before
it is written.
"""

import argparse
import cmath
import json
import math
import os
import random
import struct
import sys

FPS = 30
IDLE_SECONDS = 3.5
WALK_SECONDS = 1.0
RING = 16
LEG_RING = 8
WEIGHT_STEPS = 256
MAX_INFLUENCE_DISTANCE = 0.40
SEED = 20260930

OUT_DEFAULT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets", "capybara.glb"
)
REGENERATE = "python main/apps/render_lab/tools/gen_capybara.py"

# Name, parent, bind position. Legs bend like the animal's: elbows back,
# knees forward, so the IK below keeps the bend on the side it starts on.
JOINTS = [
    ("root", None, (0.0, 0.0, 0.0)),
    ("hips", "root", (0.0, 0.38, -0.28)),
    ("spine", "hips", (0.0, 0.37, -0.04)),
    ("chest", "spine", (0.0, 0.37, 0.15)),
    ("neck", "chest", (0.0, 0.40, 0.29)),
    ("head", "neck", (0.0, 0.42, 0.39)),
    ("ear.L", "head", (0.075, 0.522, 0.41)),
    ("ear.R", "head", (-0.075, 0.522, 0.41)),
    ("upperarm.L", "chest", (0.115, 0.30, 0.18)),
    ("forearm.L", "upperarm.L", (0.115, 0.17, 0.12)),
    ("forefoot.L", "forearm.L", (0.115, 0.05, 0.175)),
    ("upperarm.R", "chest", (-0.115, 0.30, 0.18)),
    ("forearm.R", "upperarm.R", (-0.115, 0.17, 0.12)),
    ("forefoot.R", "forearm.R", (-0.115, 0.05, 0.175)),
    ("thigh.L", "hips", (0.125, 0.32, -0.32)),
    ("shin.L", "thigh.L", (0.125, 0.18, -0.26)),
    ("hindfoot.L", "shin.L", (0.125, 0.055, -0.31)),
    ("thigh.R", "hips", (-0.125, 0.32, -0.32)),
    ("shin.R", "thigh.R", (-0.125, 0.18, -0.26)),
    ("hindfoot.R", "shin.R", (-0.125, 0.055, -0.31)),
]
JOINT_INDEX = {name: i for i, (name, _, _) in enumerate(JOINTS)}
BIND = {name: pos for name, _, pos in JOINTS}
PARENT = {name: parent for name, parent, _ in JOINTS}

# Lateral-sequence walk: footfalls LH, LF, RH, RF a quarter cycle apart.
LEGS = [
    ("hind.L", ("thigh.L", "shin.L", "hindfoot.L"), 0.00),
    ("fore.L", ("upperarm.L", "forearm.L", "forefoot.L"), 0.25),
    ("hind.R", ("thigh.R", "shin.R", "hindfoot.R"), 0.50),
    ("fore.R", ("upperarm.R", "forearm.R", "forefoot.R"), 0.75),
]
STANCE_FRACTION = 0.64
STRIDE = 0.14
STEP_HEIGHT = 0.034
# Swing folds the forefoot back at the wrist more than the hind foot at the
# ankle, so a lifted foreleg does not zig-zag at the elbow.
FOLD = {"fore": 1.1, "hind": 0.6}

# Body and head as one loft of superellipse sections across Z:
# (z, centre y, half width, height above centre, depth below, squareness).
BODY_SECTIONS = [
    (-0.505, 0.410, 0.070, 0.080, 0.090, 2.0),
    (-0.490, 0.410, 0.125, 0.140, 0.150, 2.0),
    (-0.460, 0.405, 0.165, 0.180, 0.190, 2.1),
    (-0.410, 0.400, 0.190, 0.200, 0.215, 2.2),
    (-0.340, 0.390, 0.205, 0.215, 0.225, 2.3),
    (-0.240, 0.380, 0.212, 0.210, 0.225, 2.3),
    (-0.120, 0.370, 0.212, 0.195, 0.220, 2.3),
    (0.000, 0.365, 0.205, 0.180, 0.210, 2.3),
    (0.100, 0.365, 0.190, 0.165, 0.195, 2.3),
    (0.180, 0.370, 0.170, 0.150, 0.175, 2.3),
    (0.255, 0.385, 0.145, 0.130, 0.145, 2.3),
    (0.315, 0.400, 0.128, 0.120, 0.125, 2.4),
    (0.370, 0.410, 0.132, 0.131, 0.149, 2.7),
    (0.430, 0.408, 0.136, 0.128, 0.154, 3.0),
    (0.510, 0.402, 0.129, 0.121, 0.146, 3.3),
    (0.590, 0.395, 0.117, 0.113, 0.136, 3.5),
    (0.660, 0.388, 0.107, 0.106, 0.124, 3.7),
    (0.710, 0.384, 0.100, 0.099, 0.114, 3.7),
    (0.735, 0.382, 0.088, 0.086, 0.101, 3.2),
]
BODY_NOSE_Z = 0.743
# Where each spine bone owns the body fully; weights blend linearly between.
SPINE_CENTRES = [("hips", -0.33), ("spine", -0.06), ("chest", 0.14), ("neck", 0.30), ("head", 0.42)]

SRGB = {
    "back": (82, 53, 33),
    "flank": (138, 93, 57),
    "belly": (188, 150, 106),
    "muzzle": (84, 57, 40),
    "ear": (96, 64, 45),
    "leg": (104, 71, 48),
    "foot": (60, 44, 34),
    "eye": (16, 12, 10),
    "nostril": (26, 19, 16),
}


def srgb_to_linear(rgb):
    out = []
    for c in rgb:
        c = c / 255.0
        out.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return tuple(out)


def mix(a, b, t):
    t = min(max(t, 0.0), 1.0)
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def normalize(v):
    length = math.sqrt(dot(v, v))
    return tuple(x / length for x in v)


def quat_axis(axis, angle):
    s = math.sin(angle / 2.0)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle / 2.0))


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def rotation(pitch=0.0, yaw=0.0, roll=0.0):
    """Yaw about +Y after pitch about +X after roll about +Z; +pitch dips +Z."""
    q = quat_axis((0.0, 0.0, 1.0), roll)
    q = quat_mul(quat_axis((1.0, 0.0, 0.0), pitch), q)
    return quat_mul(quat_axis((0.0, 1.0, 0.0), yaw), q)


class MeshBuilder:
    """Shells of positions, colours and joint weights, joined into one mesh."""

    def __init__(self):
        self.positions = []
        self.colors = []
        self.weights = []
        self.triangles = []
        self.shells = []

    def add_vertex(self, position, color, weights):
        self.positions.append(position)
        self.colors.append(srgb_to_linear(color))
        self.weights.append(weights)
        return len(self.positions) - 1

    def add_loft(self, rings, start_cap, end_cap):
        """Rings of equal length joined in order and capped by a fan each end.

        rings: lists of vertex indices; caps: (position, color, weights).
        """
        first_triangle = len(self.triangles)
        count = len(rings[0])
        for a, b in zip(rings, rings[1:]):
            for j in range(count):
                k = (j + 1) % count
                self.triangles.append((a[j], b[j], b[k]))
                self.triangles.append((a[j], b[k], a[k]))
        for ring, cap, flip in ((rings[0], start_cap, False), (rings[-1], end_cap, True)):
            centre = self.add_vertex(*cap)
            for j in range(count):
                k = (j + 1) % count
                tri = (centre, ring[k], ring[j]) if flip else (centre, ring[j], ring[k])
                self.triangles.append(tri)
        self._close_shell(first_triangle)

    def add_ellipsoid(self, centre, radii, turn, color, weights, rings=4, segments=8):
        first_triangle = len(self.triangles)
        cos_t, sin_t = math.cos(turn), math.sin(turn)

        def place(x, y, z):
            x, y, z = x * radii[0], y * radii[1], z * radii[2]
            x, z = x * cos_t + z * sin_t, -x * sin_t + z * cos_t
            return (centre[0] + x, centre[1] + y, centre[2] + z)

        bottom = self.add_vertex(place(0.0, -1.0, 0.0), color, weights)
        ring_indices = []
        for r in range(1, rings):
            phi = math.pi * r / rings - math.pi / 2.0
            ring_indices.append([
                self.add_vertex(
                    place(math.cos(phi) * math.cos(a), math.sin(phi), math.cos(phi) * math.sin(a)),
                    color, weights,
                )
                for a in (2.0 * math.pi * s / segments for s in range(segments))
            ])
        top = self.add_vertex(place(0.0, 1.0, 0.0), color, weights)
        for a, b in zip(ring_indices, ring_indices[1:]):
            for j in range(segments):
                k = (j + 1) % segments
                self.triangles.append((a[j], b[j], b[k]))
                self.triangles.append((a[j], b[k], a[k]))
        for j in range(segments):
            k = (j + 1) % segments
            self.triangles.append((bottom, ring_indices[0][j], ring_indices[0][k]))
            self.triangles.append((top, ring_indices[-1][k], ring_indices[-1][j]))
        self._close_shell(first_triangle)

    def _close_shell(self, first_triangle):
        """Wind the shell just added outwards, whichever way it was built."""
        tris = self.triangles[first_triangle:]
        if signed_volume(self.positions, tris) < 0.0:
            self.triangles[first_triangle:] = [(a, c, b) for a, b, c in tris]
        self.shells.append((first_triangle, len(self.triangles)))


def signed_volume(positions, triangles):
    total = 0.0
    for a, b, c in triangles:
        total += dot(positions[a], cross(positions[b], positions[c]))
    return total / 6.0


def superellipse(angle, half_width, above, below, squareness):
    c, s = math.cos(angle), math.sin(angle)
    e = 2.0 / squareness
    x = half_width * math.copysign(abs(c) ** e, c)
    y = (above if s >= 0.0 else below) * math.copysign(abs(s) ** e, s)
    return x, y


def spine_weights(z):
    names = [n for n, _ in SPINE_CENTRES]
    centres = [c for _, c in SPINE_CENTRES]
    if z <= centres[0]:
        return {names[0]: 1.0}
    if z >= centres[-1]:
        return {names[-1]: 1.0}
    for i in range(len(centres) - 1):
        if centres[i] <= z <= centres[i + 1]:
            t = smoothstep(centres[i], centres[i + 1], z)
            return {names[i]: 1.0 - t, names[i + 1]: t}
    raise AssertionError(z)


def coat_color(z, height_fraction, rng):
    color = mix(SRGB["belly"], SRGB["flank"], smoothstep(0.12, 0.45, height_fraction))
    color = mix(color, SRGB["back"], smoothstep(0.55, 0.95, height_fraction))
    color = mix(color, SRGB["muzzle"], smoothstep(0.60, 0.74, z))
    shade = 1.0 + (rng.random() - 0.5) * 0.14
    return tuple(min(255.0, c * shade) for c in color)


def build_body(mesh, rng):
    rings = []
    for index, (z, cy, w, above, below, squareness) in enumerate(BODY_SECTIONS):
        end = index in (0, len(BODY_SECTIONS) - 1)
        ring = []
        for j in range(RING):
            angle = 2.0 * math.pi * j / RING
            x, dy = superellipse(angle, w, above, below, squareness)
            fraction = (dy + below) / (above + below)
            if not end:
                # A shaggy coat: the back and flanks are tufted, the face less.
                amount = 0.011 if z < 0.36 else 0.004
                tuft = (rng.random() - 0.35) * amount * smoothstep(0.2, 0.6, fraction)
                nx, ny = normalize((x / (w * w), dy / (above * above if dy > 0 else below * below)))
                x += nx * tuft
                dy += ny * tuft
            ring.append(mesh.add_vertex((x, cy + dy, z), coat_color(z, fraction, rng), spine_weights(z)))
        rings.append(ring)
    first = BODY_SECTIONS[0]
    last = BODY_SECTIONS[-1]
    rump = ((0.0, first[1], first[0] - 0.006), SRGB["flank"], spine_weights(first[0]))
    nose = ((0.0, last[1] + 0.01, BODY_NOSE_Z), SRGB["muzzle"], spine_weights(BODY_NOSE_Z))
    mesh.add_loft(rings, rump, nose)


def body_surface_x(z, y):
    """The loft's half width at (z, y) before the coat noise, for placing parts."""
    for a, b in zip(BODY_SECTIONS, BODY_SECTIONS[1:]):
        if a[0] <= z <= b[0]:
            t = (z - a[0]) / (b[0] - a[0])
            cy, w, above, below, squareness = (a[i] + (b[i] - a[i]) * t for i in range(1, 6))
            h = above if y >= cy else below
            return w * (1.0 - min(abs(y - cy) / h, 1.0) ** squareness) ** (1.0 / squareness)
    raise AssertionError(z)


def leg_sections(upper, lower, foot, hind):
    """(centre, radius x, radius z, weights) rings from the body down to the sole."""
    u, e, f = BIND[upper], BIND[lower], BIND[foot]
    trunk = PARENT[upper]
    thick = 1.25 if hind else 1.0
    toe = 0.040 if hind else 0.028
    half = lambda a, b: tuple((x + y) / 2.0 for x, y in zip(a, b))
    return [
        ((u[0], u[1] + 0.05, u[2]), 0.046 * thick, 0.056 * thick, {upper: 0.5, trunk: 0.5}),
        (u, 0.050 * thick, 0.060 * thick, {upper: 1.0}),
        (half(u, e), 0.046 * thick, 0.053 * thick, {upper: 1.0}),
        (e, 0.044 * thick, 0.047 * thick, {upper: 0.5, lower: 0.5}),
        (half(e, f), 0.042, 0.044, {lower: 1.0}),
        (f, 0.040, 0.042, {lower: 0.5, foot: 0.5}),
        ((f[0], 0.026, f[2] + toe * 0.8), 0.046, 0.054 + toe * 0.5, {foot: 1.0}),
        ((f[0], 0.0, f[2] + toe), 0.044, 0.052 + toe * 0.5, {foot: 1.0}),
    ]


def build_leg(mesh, upper, lower, foot, hind):
    sections = leg_sections(upper, lower, foot, hind)
    rings = []
    for index, (centre, rx, rz, weights) in enumerate(sections):
        color = mix(SRGB["leg"], SRGB["flank"], smoothstep(0.10, 0.24, centre[1]))
        if index >= len(sections) - 2:
            color = SRGB["foot"]
        rings.append([
            mesh.add_vertex(
                (centre[0] + rx * math.cos(a), centre[1], centre[2] + rz * math.sin(a)), color, weights
            )
            for a in (2.0 * math.pi * j / LEG_RING for j in range(LEG_RING))
        ])
    top, bottom = sections[0], sections[-1]
    top_cap = ((top[0][0], top[0][1] + 0.02, top[0][2]), SRGB["leg"], top[3])
    sole = ((bottom[0][0], 0.0, bottom[0][2]), SRGB["foot"], bottom[3])
    mesh.add_loft(rings, top_cap, sole)


def build_face(mesh):
    head = {"head": 1.0}
    for side in (1.0, -1.0):
        ear = BIND["ear.L" if side > 0 else "ear.R"]
        mesh.add_ellipsoid(
            (ear[0] + side * 0.012, ear[1] + 0.022, ear[2]), (0.034, 0.032, 0.012),
            side * 0.55, SRGB["ear"], {"ear.L" if side > 0 else "ear.R": 1.0},
        )
        eye_z, eye_y = 0.505, 0.484
        eye_x = body_surface_x(eye_z, eye_y) - 0.006
        mesh.add_ellipsoid(
            (side * eye_x, eye_y, eye_z), (0.013, 0.014, 0.017), 0.0, SRGB["eye"], head,
            rings=4, segments=6,
        )
        mesh.add_ellipsoid(
            (side * 0.030, 0.442, BODY_NOSE_Z - 0.004), (0.015, 0.008, 0.008), side * 0.35,
            SRGB["nostril"], head, rings=3, segments=6,
        )


def quantize_weights(weights):
    """Up to 4 influences in steps of 1/256 that sum to exactly one."""
    items = sorted(weights.items(), key=lambda kv: (-kv[1], JOINT_INDEX[kv[0]]))[:4]
    total = sum(w for _, w in items)
    scaled = [(name, w / total * WEIGHT_STEPS) for name, w in items]
    steps = [(name, int(math.floor(s))) for name, s in scaled]
    remainder = WEIGHT_STEPS - sum(s for _, s in steps)
    order = sorted(range(len(scaled)), key=lambda i: -(scaled[i][1] - steps[i][1]))
    for i in order[:remainder]:
        steps[i] = (steps[i][0], steps[i][1] + 1)
    steps = [(JOINT_INDEX[name], s) for name, s in steps if s > 0]
    joints = [j for j, _ in steps] + [0] * (4 - len(steps))
    values = [s / WEIGHT_STEPS for _, s in steps] + [0.0] * (4 - len(steps))
    return joints, values


def vertex_normals(positions, triangles):
    sums = [[0.0, 0.0, 0.0] for _ in positions]
    for a, b, c in triangles:
        n = cross(sub(positions[b], positions[a]), sub(positions[c], positions[a]))
        for i in (a, b, c):
            for k in range(3):
                sums[i][k] += n[k]
    return [normalize(n) for n in sums]


def build_mesh():
    rng = random.Random(SEED)
    mesh = MeshBuilder()
    build_body(mesh, rng)
    for _, (upper, lower, foot), _ in LEGS:
        build_leg(mesh, upper, lower, foot, upper.startswith("thigh"))
    build_face(mesh)
    return mesh


def local_offset(name):
    parent = PARENT[name]
    if parent is None:
        return BIND[name]
    return sub(BIND[name], BIND[parent])


def yz(v):
    """A vector's sagittal part as y + iz: +pitch about X multiplies by e^(i*pitch)."""
    return complex(v[1], v[2])


def solve_leg(top, parent_pitch, chain, target, foot_pitch):
    """Local pitches of a leg chain whose foot joint reaches `target` (y + iz)."""
    upper, lower, foot = (yz(BIND[n]) for n in chain)
    bone1, bone2 = lower - upper, foot - lower
    length1, length2 = abs(bone1), abs(bone2)
    bend = 1.0 if cmath.phase(bone1 / (foot - upper)) > 0.0 else -1.0
    reach = target - top
    distance = min(max(abs(reach), abs(length1 - length2) + 1e-4), (length1 + length2) * 0.999)
    opening = math.acos((length1 ** 2 + distance ** 2 - length2 ** 2) / (2.0 * length1 * distance))
    thigh_angle = cmath.phase(reach) + bend * opening
    knee = top + length1 * cmath.exp(1j * thigh_angle)
    world1 = thigh_angle - cmath.phase(bone1)
    world2 = cmath.phase(target - knee) - cmath.phase(bone2)
    return world1 - parent_pitch, world2 - world1, foot_pitch - world2


def pose_frame(body, legs):
    """Full local TRS for every joint from body pitches and per-leg foot targets.

    body: {joint: (pitch, yaw, roll)} plus "lift" (hips y offset);
    legs: {leg name: (foot target y + iz, foot world pitch)}.
    """
    local = {}
    for name, _, _ in JOINTS:
        pitch, yaw, roll = body.get(name, (0.0, 0.0, 0.0))
        local[name] = [local_offset(name), rotation(pitch, yaw, roll)]
    hips = local["hips"][0]
    local["hips"][0] = (hips[0], hips[1] + body.get("lift", 0.0), hips[2])
    # Sagittal FK down the trunk; only pitch reaches the legs' parents.
    world = {"root": (0j, 0.0)}
    for name in ("hips", "spine", "chest"):
        origin, angle = world[PARENT[name]]
        pitch = body.get(name, (0.0, 0.0, 0.0))[0]
        world[name] = (origin + yz(local[name][0]) * cmath.exp(1j * angle), angle + pitch)
    for leg, chain, _ in LEGS:
        parent = PARENT[chain[0]]
        origin, angle = world[parent]
        top = origin + yz(local_offset(chain[0])) * cmath.exp(1j * angle)
        target, foot_pitch = legs[leg]
        pitches = solve_leg(top, angle, chain, target, foot_pitch)
        for name, pitch in zip(chain, pitches):
            local[name][1] = rotation(pitch)
    return local


def idle_frame(time):
    u = time / IDLE_SECONDS
    breath = math.sin(2.0 * math.pi * 2.0 * u)
    look = math.sin(2.0 * math.pi * u)

    def window(start, end):
        v = (u - start) / (end - start)
        return v if 0.0 <= v <= 1.0 else None

    dip = window(0.52, 0.72)
    nod = 0.13 * math.sin(math.pi * dip) ** 2 if dip is not None else 0.0
    flick_l = window(0.24, 0.33)
    flick_r = window(0.80, 0.87)
    ear_l = 0.55 * math.sin(4.0 * math.pi * flick_l) * (1.0 - flick_l) if flick_l is not None else 0.0
    ear_r = 0.45 * math.sin(2.0 * math.pi * flick_r) * (1.0 - flick_r) if flick_r is not None else 0.0
    drift = 0.06 * math.sin(2.0 * math.pi * u + 0.7)
    body = {
        "lift": 0.004 * breath,
        "hips": (0.004 * breath, 0.0, 0.0),
        "spine": (0.012 * breath, 0.0, 0.0),
        "chest": (-0.011 * breath, 0.0, 0.0),
        "neck": (-0.005 * breath + 0.4 * nod, 0.10 * look, 0.0),
        "head": (0.6 * nod + 0.03 * math.sin(2.0 * math.pi * u + 1.0), 0.16 * look, 0.0),
        "ear.L": (-ear_l + drift, 0.0, 0.3 * ear_l),
        "ear.R": (-ear_r - drift, 0.0, -0.3 * ear_r),
    }
    legs = {leg: (yz(BIND[chain[2]]), 0.0) for leg, chain, _ in LEGS}
    return pose_frame(body, legs)


def step_target(chain, phase, fold):
    """Foot target and pitch at a point of the cycle: plant, push back, lift, reach."""
    home = yz(BIND[chain[2]])
    if phase < STANCE_FRACTION:
        back = STRIDE * (0.5 - phase / STANCE_FRACTION)
        return home + 1j * back, 0.0
    v = (phase - STANCE_FRACTION) / (1.0 - STANCE_FRACTION)
    forward = STRIDE * (smoothstep(0.0, 1.0, v) - 0.5)
    pitch = fold * math.sin(math.pi * v) * (1.0 - v)
    # A tipped foot's toe sinks below the ankle; lift the ankle past that so
    # the foot rolls off its toe instead of through the ground.
    sole = [corner * cmath.exp(1j * pitch) for corner in sole_corners(chain)]
    sink = max(0.0, -min(c.real for c in sole) - home.real)
    lift = max(STEP_HEIGHT * math.sin(math.pi * v), sink + 0.004 * math.sin(math.pi * v))
    return home + lift + 1j * forward, pitch


def sole_corners(chain):
    """Heel and toe of a foot's sole relative to its joint, as y + iz."""
    centre, _, rz, _ = leg_sections(*chain, chain[0].startswith("thigh"))[-1]
    joint = BIND[chain[2]]
    return [complex(centre[1] - joint[1], centre[2] - joint[2] + s * rz) for s in (-1.0, 1.0)]


def walk_frame(time):
    u = time / WALK_SECONDS
    sway = math.sin(2.0 * math.pi * u)
    bob = math.cos(4.0 * math.pi * u)
    hips_pitch = 0.012 * sway
    trunk = hips_pitch - 0.006 * sway - 0.004 * sway
    body = {
        "lift": -0.007 * bob,
        "hips": (hips_pitch, 0.0, 0.0),
        "spine": (-0.006 * sway, 0.0, 0.0),
        "chest": (-0.004 * sway, 0.0, 0.0),
        "neck": (-trunk - 0.025 * bob, 0.03 * sway, 0.0),
        "head": (0.02 * bob, 0.05 * sway, 0.0),
        "ear.L": (0.08 * math.sin(4.0 * math.pi * u + 0.5), 0.0, 0.0),
        "ear.R": (0.08 * math.sin(4.0 * math.pi * u + 1.2), 0.0, 0.0),
    }
    legs = {
        leg: step_target(chain, (u - offset) % 1.0, FOLD[leg.split(".")[0]])
        for leg, chain, offset in LEGS
    }
    return pose_frame(body, legs)


def sample_animation(frame_fn, seconds):
    count = int(round(seconds * FPS))
    frames = [frame_fn(i / FPS) for i in range(count)]
    frames.append(frames[0])
    times = [i / FPS for i in range(count)] + [seconds]
    return times, frames


class GlbWriter:
    def __init__(self):
        self.blob = bytearray()
        self.views = []
        self.accessors = []

    def add(self, values, component, kind, target=None, bounds=False):
        fmt = {5126: "f", 5123: "H", 5121: "B"}[component]
        width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4, "MAT4": 16}[kind]
        while len(self.blob) % 4:
            self.blob.append(0)
        offset = len(self.blob)
        for value in values:
            self.blob += struct.pack("<" + fmt * width, *value)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(self.blob) - offset}
        if target is not None:
            view["target"] = target
        self.views.append(view)
        accessor = {
            "bufferView": len(self.views) - 1,
            "componentType": component,
            "count": len(values),
            "type": kind,
        }
        if bounds:
            rounded = [struct.unpack("<" + fmt * width, struct.pack("<" + fmt * width, *v)) for v in values]
            accessor["min"] = [min(v[k] for v in rounded) for k in range(width)]
            accessor["max"] = [max(v[k] for v in rounded) for k in range(width)]
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def encode(mesh, animations):
    writer = GlbWriter()
    normals = vertex_normals(mesh.positions, mesh.triangles)
    skin = [quantize_weights(w) for w in mesh.weights]
    attributes = {
        "POSITION": writer.add(mesh.positions, 5126, "VEC3", 34962, bounds=True),
        "NORMAL": writer.add(normals, 5126, "VEC3", 34962),
        "COLOR_0": writer.add(mesh.colors, 5126, "VEC3", 34962),
        "JOINTS_0": writer.add([j for j, _ in skin], 5121, "VEC4", 34962),
        "WEIGHTS_0": writer.add([w for _, w in skin], 5126, "VEC4", 34962),
    }
    indices = writer.add([(i,) for tri in mesh.triangles for i in tri], 5123, "SCALAR", 34963)
    inverse_binds = []
    for name, _, (x, y, z) in JOINTS:
        inverse_binds.append((1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1))
    inverse_accessor = writer.add(inverse_binds, 5126, "MAT4")

    nodes = []
    for name, parent, _ in JOINTS:
        node = {"name": name, "translation": list(local_offset(name))}
        children = [JOINT_INDEX[n] for n, p, _ in JOINTS if p == name]
        if children:
            node["children"] = children
        nodes.append(node)
    mesh_node = len(nodes)
    nodes.append({"name": "capybara", "mesh": 0, "skin": 0})

    gltf_animations = []
    for anim_name, (times, frames) in animations:
        time_accessor = writer.add([(t,) for t in times], 5126, "SCALAR", bounds=True)
        samplers, channels = [], []
        for name, _, _ in JOINTS:
            if name == "root":
                continue
            for path, slot, kind in (("translation", 0, "VEC3"), ("rotation", 1, "VEC4")):
                values = [tuple(frame[name][slot]) for frame in frames]
                if path == "translation" and all(v == values[0] for v in values):
                    continue
                if path == "rotation":
                    values = continuous_quaternions(values)
                output = writer.add(values, 5126, kind)
                samplers.append({"input": time_accessor, "output": output, "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": JOINT_INDEX[name], "path": path}})
        gltf_animations.append({"name": anim_name, "samplers": samplers, "channels": channels})

    document = {
        "asset": {
            "version": "2.0",
            "generator": "autana gen_capybara.py",
            "extras": {"note": "GENERATED FILE - regenerate with: " + REGENERATE},
        },
        "scene": 0,
        "scenes": [{"name": "capybara", "nodes": [JOINT_INDEX["root"], mesh_node]}],
        "nodes": nodes,
        "meshes": [{"name": "capybara", "primitives": [
            {"attributes": attributes, "indices": indices, "mode": 4}
        ]}],
        "skins": [{
            "name": "capybara",
            "joints": list(range(len(JOINTS))),
            "skeleton": JOINT_INDEX["root"],
            "inverseBindMatrices": inverse_accessor,
        }],
        "animations": gltf_animations,
        "accessors": writer.accessors,
        "bufferViews": writer.views,
        "buffers": [{"byteLength": len(writer.blob)}],
    }
    return pack_glb(document, bytes(writer.blob))


def continuous_quaternions(values):
    """Flip signs so neighbouring keys take the short way; the loop keys stay equal."""
    out = [values[0]]
    for q in values[1:]:
        if dot(q, out[-1]) < 0.0:
            q = tuple(-x for x in q)
        out.append(q)
    if out[-1] != out[0]:
        raise SystemExit("rotation loop does not close")
    return out


def pack_glb(document, binary):
    text = json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8")
    text += b" " * (-len(text) % 4)
    binary += b"\0" * (-len(binary) % 4)
    length = 12 + 8 + len(text) + 8 + len(binary)
    return (
        struct.pack("<III", 0x46546C67, 2, length)
        + struct.pack("<II", len(text), 0x4E4F534A) + text
        + struct.pack("<II", len(binary), 0x004E4942) + binary
    )


def point_segment_distance(p, a, b):
    ab = sub(b, a)
    t = 0.0 if dot(ab, ab) == 0.0 else min(max(dot(sub(p, a), ab) / dot(ab, ab), 0.0), 1.0)
    closest = tuple(x + y * t for x, y in zip(a, ab))
    d = sub(p, closest)
    return math.sqrt(dot(d, d))


def joint_distance(name, p):
    """Distance from a point to a joint's bone: the joint and its children."""
    children = [n for n, parent, _ in JOINTS if parent == name] or [name]
    return min(point_segment_distance(p, BIND[name], BIND[c]) for c in children)


def validate(mesh, animations):
    edges = {}
    for a, b, c in mesh.triangles:
        for e in ((a, b), (b, c), (c, a)):
            edges[e] = edges.get(e, 0) + 1
    for (a, b), count in edges.items():
        if count != 1 or edges.get((b, a)) != 1:
            raise SystemExit("mesh is not closed and consistently wound at edge %d-%d" % (a, b))
    for start, end in mesh.shells:
        if signed_volume(mesh.positions, mesh.triangles[start:end]) <= 0.0:
            raise SystemExit("a shell is wound inwards")
    if len(mesh.positions) > 65535:
        raise SystemExit("too many vertices for 16-bit indices")
    for p, weights in zip(mesh.positions, mesh.weights):
        for name, w in weights.items():
            if w > 0.0 and joint_distance(name, p) > MAX_INFLUENCE_DISTANCE:
                raise SystemExit("vertex %s is bound to distant joint %s" % (p, name))
    for name, (times, frames) in animations:
        if frames[0] != frames[-1] or len(times) != len(frames):
            raise SystemExit("animation %s does not loop" % name)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=OUT_DEFAULT, help="output .glb path")
    args = parser.parse_args()
    mesh = build_mesh()
    animations = [
        ("idle", sample_animation(idle_frame, IDLE_SECONDS)),
        ("walk", sample_animation(walk_frame, WALK_SECONDS)),
    ]
    validate(mesh, animations)
    data = encode(mesh, animations)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as handle:
        handle.write(data)
    speed = STRIDE / (STANCE_FRACTION * WALK_SECONDS)
    print(
        "%s: %d vertices, %d triangles, %d joints, idle %.2f s, walk %.2f s (treadmill %.2f m/s)"
        % (args.out, len(mesh.positions), len(mesh.triangles), len(JOINTS), IDLE_SECONDS, WALK_SECONDS, speed),
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
