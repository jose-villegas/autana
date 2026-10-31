#!/usr/bin/env python3
"""Generate main/boot/boot_anim_timeline.h: the boot animation's timing
constants, scalar settings and baked floor tables.

    python tools/gen/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json main/boot/boot_anim_motion.glb > main/boot/boot_anim_timeline.h

WHERE THIS DATA COMES FROM

main/boot/boot_anim_timeline.json is the source of truth for everything
that is not motion, edited by hand. The camera and the space the grid and
curve live in move by a glTF animation instead,
main/boot/boot_anim_motion.glb (see docs/render/animation/Animation-Tracks.md), authored in
any glTF tool and baked into the boot clip's own asset pack by
tools/r3d/build_pack.py from main/boot/boot_anim_motion.anim.toml; this
script only reads it to check it.

The output header includes the floor's distance bands, Gaussian and calm
lookups, packet frame scalars and swell phases. The mesh geometry and its radial bounds are
baked with the timeline. Only camera transforms and drawing depend on the view.

VALIDATION

Refuses to emit anything that would draw something broken (curve still being
drawn when the fade starts, a motion that ends off the timeline) the same way
gen_zeta_curve.py refuses to ship a curve that fails its own zero check.
Aesthetic-only concerns (a title letter still flying when the fade starts)
are a warning, not a refusal; unlike a broken curve, that might be exactly
what someone editing the timeline wants.
"""

import json
import math
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf import gltf_read  # noqa: E402
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "device"))
from panel_size import PANEL_WIDTH, PANEL_HEIGHT

MOTION_ANIMATION = "boot_motion"
MOTION_NODES = ("camera", "space")

# Must match BOOT_ANIM_TITLE_LEN in boot_anim.h, only used for the
# soft landing-before-fade warning below, not emitted anywhere.
TITLE_LEN = 6


def fail(msg):
    sys.exit("gen_boot_anim_timeline.py: " + msg)


def warn(msg):
    print("gen_boot_anim_timeline.py: warning: " + msg, file=sys.stderr)


# One space-unit is one meter, see boot_anim.h's own top comment, so this
# is the SAME conversion boot_anim.c's units() does for the curve/grid's own
# geometry, not the camera's float meters. BOOT_ANIM_ONE, not
# imported from boot_anim.h to keep this script standalone.
BOOT_ANIM_ONE = 4096


def meters_to_q12(v):
    return round(v * BOOT_ANIM_ONE)


def mesh_step_q12(cfg):
    """One mesh cell, so mesh_cells / 2 of them reach grid_bands bands."""
    reach_q12 = meters_to_q12(cfg["grid_step_m"]) * cfg["timing"]["grid_bands"]
    return round(reach_q12 / (cfg["timing"]["mesh_cells"] // 2))


def load_motion(path):
    """{"duration": seconds} of the glTF animation the firmware bakes for the
    camera and the space."""
    document, binary = gltf_read.load_asset(path)
    animation = next((a for a in document.get("animations", [])
                      if a.get("name") == MOTION_ANIMATION), None)
    if animation is None:
        fail("%s has no animation named %r" % (path, MOTION_ANIMATION))
    channels = gltf_read.read_animation(document, binary, animation)
    animated = set()
    for channel in channels:
        node = document["nodes"][channel["node"]].get("name") if channel["node"] is not None else None
        if node in MOTION_NODES:
            animated.add(node)
    for node in MOTION_NODES:
        if node not in animated:
            fail("%s: the %r node has no animated channel" % (path, node))
    return {"duration": gltf_read.animation_duration(channels)}


def validate_motion(motion, timing):
    last_ms = round(motion["duration"] * 1000)
    if last_ms != timing["total_ms"]:
        warn("the last motion key is at %d, not total_ms (%d) - the camera "
             "and space will hold their last values for the remainder"
             % (last_ms, timing["total_ms"]))


FLOOR_SHAPE_SCALE = 256
FLOOR_TIME_STEP_MS = 10
FLOOR_GAIN_ONE = 32768
FLOOR_BAND_SHIFT = 4
FLOOR_PHASE_TURN = 65536
FLOOR_GAUSSIAN_LIMIT = 6
FLOOR_LOOKUP_SCALE = 256


def floor_number(value, name, minimum=0, positive=False, integer=False):
    if (not isinstance(value, (float, int)) or isinstance(value, bool) or
            not math.isfinite(value) or (minimum is not None and value < minimum) or
            (positive and value == minimum) or (integer and int(value) != value)):
        fail(name + " must be " + ("positive " if positive else "nonnegative " if minimum is not None else "") +
             ("finite integer" if integer else "finite number"))


def validate_floor(cfg):
    allowed = {"timing", "camera_half_fov_short_tan", "grid_step_m",
               "ripple_wavelength_m", "ripple_clock_s", "ripple_group_speed_m_s",
               "ripple_width_m", "ripple_dispersion_m_s", "ripple_source_radius_m",
               "ripple_calm_fraction", "crest_height_m", "crest_colour_stride", "grid_level", "floor_grey",
               "far_m", "far_grow_power", "far_line_cells", "far_line_growth", "far_front",
               "far_front_steps", "far_swell_fade_m", "far_swell_out_ms",
               "grid_edge_fade_fraction",
               "ripple_height_m", "swell_height_m", "swell"}
    for key in sorted(set(cfg) - allowed):
        fail("unknown key: " + key)
    for key in ("ripple_wavelength_m", "ripple_clock_s", "ripple_group_speed_m_s",
                "ripple_width_m", "ripple_dispersion_m_s", "ripple_source_radius_m", "crest_height_m"):
        floor_number(cfg.get(key), key, positive=True)
    for key in ("ripple_height_m", "swell_height_m"):
        floor_number(cfg.get(key), key)
    for key in ("ripple_impact_ms", "ripple_calm_after_ms", "swell_in_ms"):
        floor_number(cfg["timing"].get(key), key,
                     positive=key == "swell_in_ms", integer=True)
    if not isinstance(cfg.get("swell"), list) or not cfg["swell"]:
        fail("swell must be a nonempty list")
    for index, sine in enumerate(cfg["swell"]):
        if not isinstance(sine, dict):
            fail("swell[%d] must be an object" % index)
        for key in ("wavelength_m", "period_ms", "direction_deg", "weight"):
            floor_number(sine.get(key), "swell[%d].%s" % (index, key),
                         minimum=None if key == "direction_deg" else 0,
                         positive=key in ("wavelength_m", "period_ms"),
                         integer=key == "period_ms")
        if sine["period_ms"] <= 2 * FLOOR_TIME_STEP_MS:
            fail("swell[%d].period_ms must exceed twice the baked time step" % index)
        for key in sorted(set(sine) - {"wavelength_m", "period_ms", "direction_deg", "weight"}):
            fail("unknown swell key: " + key)
    amplitude = cfg["ripple_height_m"] + cfg["swell_height_m"] * sum(s["weight"] for s in cfg["swell"])
    if meters_to_q12(amplitude) >= (1 << 15):
        fail("ripple_height_m + weighted swell_height_m must fit signed Q12")
    for key in ("ripple_calm_fraction", "grid_edge_fade_fraction"):
        floor_number(cfg.get(key), key)
        if cfg[key] >= 1:
            fail(key + " must be less than 1")
    if round(cfg["grid_edge_fade_fraction"] * cfg["timing"]["grid_bands"]) >= cfg["timing"]["grid_bands"]:
        fail("grid_edge_fade_fraction must leave a fade band")
    if not 0 < meters_to_q12(cfg["crest_height_m"]) < (1 << 15):
        fail("crest_height_m must fit positive signed Q12")
    floor_number(cfg.get("crest_colour_stride"), "crest_colour_stride", positive=True, integer=True)
    if (cfg["timing"]["mesh_cells"] // 2) % cfg["crest_colour_stride"]:
        fail("crest_colour_stride must divide mesh_cells / 2")
    floor_number(cfg.get("grid_level"), "grid_level", positive=True)
    if cfg["grid_level"] > 1:
        fail("grid_level must be at most 1")
    floor_number(cfg.get("floor_grey"), "floor_grey", integer=True)
    if cfg["floor_grey"] > 0xFFFFFF:
        fail("floor_grey must fit RGB24")
    for key in ("ripple_width_m", "ripple_wavelength_m"):
        if FLOOR_GAIN_ONE / cfg[key] >= (1 << 16):
            fail(key + " must fit the unsigned reciprocal index")
    reach = math.sqrt(2) * mesh_step_q12(cfg) * (cfg["timing"]["mesh_cells"] // 2)
    travel = cfg["ripple_group_speed_m_s"] * cfg["ripple_clock_s"] * FLOOR_SHAPE_SCALE
    if not math.isfinite(travel) or travel >= (1 << 15):
        fail("ripple_group_speed_m_s * ripple_clock_s must fit the signed travel index")
    if reach * FLOOR_SHAPE_SCALE / BOOT_ANIM_ONE >= (1 << 16):
        fail("grid_step_m * grid_bands must fit the unsigned radius index")
    for key in ("mesh_dash_on_px", "mesh_dash_off_px"):
        floor_number(cfg["timing"].get(key), key, positive=True, integer=True)
        if cfg["timing"][key] > 255:
            fail(key + " must fit an unsigned byte")
    floor_number(cfg.get("far_m"), "far_m", positive=True)
    if meters_to_q12(cfg["far_m"]) <= mesh_step_q12(cfg) * (cfg["timing"]["mesh_cells"] // 2):
        fail("far_m must reach past the mesh's edge")
    if meters_to_q12(cfg["far_m"]) * FAR_CAMERA_SCALE >= (1 << 31):
        fail("far_m must fit the camera transform's 32-bit input")
    floor_number(cfg.get("far_grow_power"), "far_grow_power", positive=True)
    floor_number(cfg.get("far_line_cells"), "far_line_cells", positive=True, integer=True)
    floor_number(cfg.get("far_line_growth"), "far_line_growth", positive=True)
    if cfg["far_line_growth"] <= 1:
        fail("far_line_growth must be greater than 1")
    floor_number(cfg.get("far_front"), "far_front")
    if cfg["far_front"] >= 1:
        fail("far_front must be less than 1")
    floor_number(cfg.get("far_front_steps"), "far_front_steps", positive=True, integer=True)
    floor_number(cfg.get("far_swell_fade_m"), "far_swell_fade_m", positive=True)
    edge = mesh_step_q12(cfg) * (cfg["timing"]["mesh_cells"] // 2)
    fade = meters_to_q12(cfg["far_swell_fade_m"])
    if not edge < fade < meters_to_q12(cfg["far_m"]):
        fail("far_swell_fade_m must lie between the mesh's edge and far_m")
    if far_sample_gap(cfg, fade) * 2 > meters_to_q12(min(s["wavelength_m"] for s in cfg["swell"])):
        fail("far_swell_fade_m must end the swell before the far samples are more than half its shortest "
             "wavelength apart")
    floor_number(cfg.get("far_swell_out_ms"), "far_swell_out_ms", positive=True, integer=True)


def floor_array(w, ctype, name, rows):
    w("static const %s %s[] = {\n" % (ctype, name))
    for row in rows:
        w("    {" + ", ".join(str(v) for v in row) + "},\n")
    w("};\n\n")


def emit_floor(cfg, w):
    timing = cfg["timing"]
    half = timing["mesh_cells"] // 2
    step = mesh_step_q12(cfg)
    band = meters_to_q12(cfg["grid_step_m"])
    for key in ("ripple_wavelength_m", "ripple_clock_s", "ripple_group_speed_m_s", "ripple_width_m",
                "ripple_dispersion_m_s", "ripple_source_radius_m", "ripple_calm_fraction",
                "ripple_height_m", "swell_height_m"):
        w("#define BOOT_ANIM_%s %s\n" % (key.upper(), repr(float(cfg[key]))))
    w("#define BOOT_ANIM_FLOOR_GREY 0x%06X\n" % cfg["floor_grey"])
    w("#define BOOT_ANIM_CREST_HEIGHT_Q12 %d\n" % meters_to_q12(cfg["crest_height_m"]))
    w("#define BOOT_ANIM_GRID_LEVEL %d\n" % round(cfg["grid_level"] * 255))
    w("#define BOOT_ANIM_GRID_EDGE_FADE_BAND %d\n" % round(cfg["grid_edge_fade_fraction"] * timing["grid_bands"]))
    w("#define BOOT_ANIM_RIPPLE_WAVELENGTH_SHAPE %d\n" % round(cfg["ripple_wavelength_m"] * FLOOR_SHAPE_SCALE))
    w("#define BOOT_ANIM_RIPPLE_INV_WAVELENGTH %d\n" % round(FLOOR_GAIN_ONE / cfg["ripple_wavelength_m"]))
    w("#ifndef DEVICE_BUILD\n")
    w("typedef struct { double wavelength_m, period_ms, direction_deg, weight; } boot_anim_swell_setting_t;\n")
    w("static const boot_anim_swell_setting_t boot_anim_swell_settings[] = {\n")
    for sine in cfg["swell"]:
        w("    {" + ", ".join(repr(float(sine[key])) for key in ("wavelength_m", "period_ms", "direction_deg", "weight")) + "},\n")
    w("};\n#endif\n")
    w("#define BOOT_ANIM_FLOOR_BAND_SHIFT %d\n" % FLOOR_BAND_SHIFT)
    w("#define BOOT_ANIM_RIPPLE_SHAPE_SCALE %d\n" % FLOOR_SHAPE_SCALE)
    w("#define BOOT_ANIM_RIPPLE_GAUSSIAN_EXTENT %d\n" % (FLOOR_GAUSSIAN_LIMIT * FLOOR_LOOKUP_SCALE))
    w("#define BOOT_ANIM_RIPPLE_LOOKUP_SCALE %d\n" % FLOOR_LOOKUP_SCALE)
    w("#define BOOT_ANIM_FLOOR_TIME_STEP_MS %d\n" % FLOOR_TIME_STEP_MS)
    w("#define BOOT_ANIM_FLOOR_GAIN_SHIFT 15\n")
    w("#define BOOT_ANIM_SWELL_COUNT %d\n\n" % len(cfg["swell"]))
    w("typedef struct { uint16_t radius, band_q4, phase; } boot_anim_floor_node_t;\n")
    rows = []
    for j in range(half + 1):
        for i in range(half + 1):
            r_q12 = math.isqrt((i * step)**2 + (j * step)**2)
            r = r_q12 / BOOT_ANIM_ONE
            rows.append((round(r * FLOOR_SHAPE_SCALE), (r_q12 << FLOOR_BAND_SHIFT) // band,
                         round(r * FLOOR_PHASE_TURN / cfg["ripple_wavelength_m"]) % FLOOR_PHASE_TURN))
    floor_array(w, "boot_anim_floor_node_t", "boot_anim_floor_nodes", rows)
    for name, values in (
            ("gaussian", [round(FLOOR_GAIN_ONE * math.exp(-0.5 * (n / FLOOR_LOOKUP_SCALE)**2))
                          for n in range(FLOOR_GAUSSIAN_LIMIT * FLOOR_LOOKUP_SCALE + 1)]),
            ("smoothstep", [round(FLOOR_GAIN_ONE * (n / FLOOR_LOOKUP_SCALE)**2 * (3 - 2 * n / FLOOR_LOOKUP_SCALE))
                            for n in range(FLOOR_LOOKUP_SCALE + 1)])):
        w("static const uint16_t boot_anim_ripple_%s[] = {\n" % name)
        for start in range(0, len(values), 16):
            w("    " + ", ".join(map(str, values[start:start + 16])) + ",\n")
        w("};\n\n")
    w("typedef struct { int16_t radius, amplitude, calm_start; uint16_t inv_width, ripple_phase, swell, far_swell; uint16_t phase[BOOT_ANIM_SWELL_COUNT]; int32_t far_reach; } boot_anim_floor_time_t;\n")
    w("static const boot_anim_floor_time_t boot_anim_floor_times[] = {\n")
    for ms in range(0, timing["total_ms"] + FLOOR_TIME_STEP_MS * 2, FLOOR_TIME_STEP_MS):
        tau = max(0, ms - timing["ripple_impact_ms"]) / 1000
        clock = cfg["ripple_clock_s"]
        radius = cfg["ripple_group_speed_m_s"] * clock * (1 - math.exp(-tau / clock))
        sigma = math.hypot(cfg["ripple_width_m"], cfg["ripple_dispersion_m_s"] * tau)
        amplitude = cfg["ripple_height_m"] * math.sqrt(cfg["ripple_source_radius_m"] * cfg["ripple_width_m"] /
                    ((radius + cfg["ripple_source_radius_m"]) * sigma))
        calm_start = radius * cfg["ripple_calm_fraction"] - cfg["ripple_wavelength_m"] / 2
        ripple_phase = round(-2 * radius * FLOOR_PHASE_TURN / cfg["ripple_wavelength_m"]) % FLOOR_PHASE_TURN
        gain = round(FLOOR_GAIN_ONE * min(1, max(0, (ms - timing["title_start_ms"]) / timing["swell_in_ms"])))
        phases = [round(-ms * FLOOR_PHASE_TURN / s["period_ms"]) % FLOOR_PHASE_TURN for s in cfg["swell"]]
        scalars = (round(radius * FLOOR_SHAPE_SCALE), meters_to_q12(amplitude), round(calm_start * FLOOR_SHAPE_SCALE),
                   round(FLOOR_GAIN_ONE / sigma), ripple_phase, gain, far_swell_time_gain(cfg, ms))
        if any(not -(1 << 15) <= v < (1 << 15) for v in scalars[:3]):
            fail("ripple frame scalars must fit signed 16-bit fields")
        w("    {%s, {%s}, %d},\n" % (", ".join(map(str, scalars)), ", ".join(map(str, phases)), far_reach(cfg, ms)))
    w("};\n\n")
    weights = [meters_to_q12(cfg["swell_height_m"] * s["weight"]) for s in cfg["swell"]]
    w("static const int16_t boot_anim_swell_weights[] = {" + ", ".join(map(str, weights)) + "};\n")
    for axis, component in (("re", math.cos), ("im", math.sin)):
        rows = []
        for k in range(-half, half + 1):
            rows.append([round(k * step / BOOT_ANIM_ONE * component(math.radians(s["direction_deg"])) / s["wavelength_m"] * FLOOR_PHASE_TURN) % FLOOR_PHASE_TURN for s in cfg["swell"]])
        w("static const uint16_t boot_anim_swell_%s[][BOOT_ANIM_SWELL_COUNT] = {\n" % axis)
        for row in rows:
            w("    {" + ", ".join(map(str, row)) + "},\n")
        w("};\n\n")
    rows = []
    for stride in (1, 2):
        reach = timing["grid_bands"] * band + stride * step
        rows.append([min(half, math.isqrt(max(0, reach**2 - (j * step)**2)) // step) for j in range(half + 1)])
    w("static const uint8_t boot_anim_mesh_row_half[2][%d] = {\n" % (half + 1))
    for row in rows:
        w("    {" + ", ".join(map(str, row)) + "},\n")
    w("};\n")


# The curve the pen draws, read from its own generated header: the crest
# colours follow its samples, so they must be the very points shipped.
CURVE_HEADER = pathlib.Path(__file__).resolve().parents[2] / "main" / "boot" / "boot_anim_curve.h"


def load_curve_plane():
    text = CURVE_HEADER.read_text(encoding="utf-8")
    body = text[text.index("boot_anim_curve[BOOT_ANIM_CURVE_POINTS]"):]
    body = body[:body.index("};")]
    points = [(int(x), int(y)) for x, y, _ in re.findall(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", body)]
    if not points:
        fail("no curve samples in " + str(CURVE_HEADER))
    return points


def crest_runs(cfg):
    """For every crest colour node, the runs of curve samples that come
    nearer to it than every sample before them: while the pen is inside a
    run, the nearest drawn sample is the pen's own; between runs it holds."""
    half = cfg["timing"]["mesh_cells"] // 2
    stride = cfg["crest_colour_stride"]
    step = mesh_step_q12(cfg)
    points = load_curve_plane()
    starts, runs = [], []
    for cj in range(-half, half + 1, stride):
        for ci in range(-half, half + 1, stride):
            x, y = ci * step, cj * step
            starts.append(len(runs))
            best = None
            for k, (px, py) in enumerate(points):
                d = (px - x) ** 2 + (py - y) ** 2
                if best is None or d < best:
                    best = d
                    if len(runs) > starts[-1] and runs[-1][1] == k - 1:
                        runs[-1][1] = k
                    else:
                        runs.append([k, k])
    starts.append(len(runs))
    if len(runs) >= (1 << 16) or len(points) >= (1 << 16):
        fail("crest runs must fit unsigned 16-bit indices")
    return starts, runs


# boot_anim_to_camera_space() scales a Q12 floor coordinate by this before a
# 32-bit multiply; far_m must stay inside it.
FAR_CAMERA_SCALE = 16
# Past the mesh the axis lines take a sample every mesh step, then every
# sixteenth of the distance from the origin, as far apart on screen as the
# distance allows.
FAR_SAMPLE_DIVISOR = 16


def far_reach(cfg, ms):
    """How far out the floor reaches at `ms`, Q12: from the mesh's edge to
    far_m over far_grow_ms once the mesh has arrived, eased in by
    far_grow_power so the near plane, most of the screen, fills slowly."""
    timing = cfg["timing"]
    edge = mesh_step_q12(cfg) * (timing["mesh_cells"] // 2)
    start = timing["grid_start_ms"] + timing["grid_bands"] * timing["grid_band_ms"]
    t = min(1.0, max(0.0, (ms - start) / timing["far_grow_ms"]))
    return round(edge + (meters_to_q12(cfg["far_m"]) - edge) * t ** cfg["far_grow_power"])


def far_sample_gap(cfg, r_q12):
    """The gap after the far sample at `r_q12`."""
    return max(mesh_step_q12(cfg), r_q12 // FAR_SAMPLE_DIVISOR)


def axis_far_samples(cfg):
    edge = mesh_step_q12(cfg) * (cfg["timing"]["mesh_cells"] // 2)
    far = meters_to_q12(cfg["far_m"])
    radii = [edge]
    while radii[-1] < far:
        radii.append(min(far, radii[-1] + far_sample_gap(cfg, radii[-1])))
    return radii


def far_swell_gain(cfg, r_q12):
    """How much of the swell the floor keeps at `r_q12`, in FLOOR_GAIN_ONE:
    all of it at the mesh's edge, easing to none at far_swell_fade_m, so the
    horizon stays flat and the samples never thin past the waves."""
    edge = mesh_step_q12(cfg) * (cfg["timing"]["mesh_cells"] // 2)
    fade = meters_to_q12(cfg["far_swell_fade_m"])
    u = min(1.0, max(0.0, (fade - r_q12) / (fade - edge)))
    return round(FLOOR_GAIN_ONE * u * u * (3 - 2 * u))


def far_swell_time_gain(cfg, ms):
    """How much of the swell the far grid keeps at `ms`, in FLOOR_GAIN_ONE:
    all of it until far_swell_out_ms before the photograph starts crossing
    in, easing to none by the last time step at or before it, so the
    crossfade draws the far grid flat."""
    end = cfg["timing"]["image_start_ms"] // FLOOR_TIME_STEP_MS * FLOOR_TIME_STEP_MS
    u = min(1.0, max(0.0, (end - ms) / cfg["far_swell_out_ms"]))
    return round(FLOOR_GAIN_ONE * u * u * (3 - 2 * u))


def swell_phases(cfg, d_q12, component):
    """The swell's spatial phases `d_q12` out along +re (cos) or +im (sin)."""
    return [round(d_q12 / BOOT_ANIM_ONE * component(math.radians(sine["direction_deg"])) / sine["wavelength_m"]
                  * FLOOR_PHASE_TURN) % FLOOR_PHASE_TURN for sine in cfg["swell"]]


def emit_axis_far(cfg, w):
    """The axis lines past the mesh, one row per sample out along +re and +im:
    its distance, its ripple node, its swell phases and how much of the swell
    it keeps. The negative directions negate the phases; the first sample is
    the mesh's edge node."""
    radii = axis_far_samples(cfg)
    w("\n#define BOOT_ANIM_FAR_Q12 %d\n" % meters_to_q12(cfg["far_m"]))
    w("#define BOOT_ANIM_FAR_SWELL_FADE_Q12 %d\n" % meters_to_q12(cfg["far_swell_fade_m"]))
    w("#define BOOT_ANIM_FAR_SWELL_OUT_MS %d\n" % cfg["far_swell_out_ms"])
    w("#define BOOT_ANIM_AXIS_FAR_SAMPLES %d\n\n" % len(radii))
    w("typedef struct { int32_t r; boot_anim_floor_node_t node; uint16_t swell_re[BOOT_ANIM_SWELL_COUNT], "
      "swell_im[BOOT_ANIM_SWELL_COUNT]; uint16_t swell_gain; } boot_anim_axis_far_t;\n")
    w("static const boot_anim_axis_far_t boot_anim_axis_far[] = {\n")
    for r_q12 in radii:
        r = r_q12 / BOOT_ANIM_ONE
        node = (min(round(r * FLOOR_SHAPE_SCALE), (1 << 16) - 1), 0,
                round(r * FLOOR_PHASE_TURN / cfg["ripple_wavelength_m"]) % FLOOR_PHASE_TURN)
        phases = [swell_phases(cfg, r_q12, component) for component in (math.cos, math.sin)]
        w("    {%d, {%d, %d, %d}, {%s}, {%s}, %d},\n" % ((r_q12,) + node + (", ".join(map(str, phases[0])),
                                                         ", ".join(map(str, phases[1])),
                                                         far_swell_gain(cfg, r_q12))))
    w("};\n")


def far_lines(cfg):
    """Where the far grid's lines cross the axis, out from the origin: every
    far_line_cells-th mesh line inside the mesh's edge, then the gap doubling
    each time the distance grows by far_line_growth, since far lines crowd on
    screen anyway."""
    edge = mesh_step_q12(cfg) * (cfg["timing"]["mesh_cells"] // 2)
    far = meters_to_q12(cfg["far_m"])
    lines = []
    c = 0
    while True:
        spacing = cfg["far_line_cells"] * mesh_step_q12(cfg)
        band = edge
        while c >= band:
            band *= cfg["far_line_growth"]
            spacing *= 2
        c += spacing
        if c > far:
            return lines
        lines.append(c)


def far_line_samples(cfg, c):
    """Where far line `c` is sampled for the swell, as (distance along it
    from the axis, distance from the origin): where it leaves the mesh, where
    it crosses each axis sample's circle, and where the swell has faded out.
    Empty for a line wholly past the fade."""
    edge = mesh_step_q12(cfg) * (cfg["timing"]["mesh_cells"] // 2)
    fade = meters_to_q12(cfg["far_swell_fade_m"])
    if c >= fade:
        return []
    start = max(edge, c)
    radii = [start] + [r for r in axis_far_samples(cfg) if start < r < fade] + [fade]
    return [(math.isqrt(r * r - c * c), r) for r in radii]


def emit_far_grid(cfg, w):
    """The far lines, each with its swell phases across the axis and its run
    of boot_anim_far_samples: the distance along the line, the phases there
    and how much of the swell it keeps. A line at -c, or the half running the
    other way, negates the phases."""
    lines = far_lines(cfg)
    w("\n#define BOOT_ANIM_FAR_FRONT_Q8 %d\n" % round(cfg["far_front"] * 256))
    w("#define BOOT_ANIM_FAR_FRONT_STEPS %d\n" % cfg["far_front_steps"])
    w("#define BOOT_ANIM_FAR_LINES %d\n\n" % len(lines))
    w("typedef struct { int32_t s; uint16_t swell_re[BOOT_ANIM_SWELL_COUNT], swell_im[BOOT_ANIM_SWELL_COUNT]; "
      "uint16_t swell_gain; } boot_anim_far_sample_t;\n")
    w("static const boot_anim_far_sample_t boot_anim_far_samples[] = {\n")
    runs = []
    for c in lines:
        samples = far_line_samples(cfg, c)
        if len(samples) > 255:
            fail("a far line must take at most 255 swell samples")
        runs.append((sum(count for _, count in runs), len(samples)))
        for s, r in samples:
            w("    {%d, {%s}, {%s}, %d},\n" % (s, ", ".join(map(str, swell_phases(cfg, s, math.cos))),
                                             ", ".join(map(str, swell_phases(cfg, s, math.sin))),
                                             far_swell_gain(cfg, r)))
    w("};\n\n")
    w("typedef struct { int32_t c; uint16_t swell_re[BOOT_ANIM_SWELL_COUNT], swell_im[BOOT_ANIM_SWELL_COUNT]; "
      "uint16_t first; uint8_t count; } boot_anim_far_line_t;\n")
    w("static const boot_anim_far_line_t boot_anim_far_lines[BOOT_ANIM_FAR_LINES] = {\n")
    for c, (first, count) in zip(lines, runs):
        w("    {%d, {%s}, {%s}, %d, %d},\n" % (c, ", ".join(map(str, swell_phases(cfg, c, math.cos))),
                                             ", ".join(map(str, swell_phases(cfg, c, math.sin))), first, count))
    w("};\n")


def emit_crest(cfg, w):
    starts, runs = crest_runs(cfg)
    side = cfg["timing"]["mesh_cells"] // cfg["crest_colour_stride"] + 1
    w("\n#define BOOT_ANIM_CREST_STRIDE %d\n" % cfg["crest_colour_stride"])
    w("#define BOOT_ANIM_CREST_SIDE %d\n" % side)
    w("#define BOOT_ANIM_CREST_MAX_RUNS %d\n\n" % max(b - a for a, b in zip(starts, starts[1:])))
    w("static const uint16_t boot_anim_crest_run_start[%d] = {\n" % len(starts))
    for at in range(0, len(starts), 16):
        w("    " + ", ".join(map(str, starts[at:at + 16])) + ",\n")
    w("};\n\n")
    w("static const uint16_t boot_anim_crest_runs[%d][2] = {\n" % len(runs))
    for at in range(0, len(runs), 8):
        w("    " + ", ".join("{%d, %d}" % tuple(r) for r in runs[at:at + 8]) + ",\n")
    w("};\n")


def validate(cfg):
    timing = cfg["timing"]
    unknown = sorted(set(timing) - {key for key, _, _ in TIMING_ORDER})
    if unknown:
        fail("unknown timing key(s): %s - not a setting of this timeline" %
             ", ".join(unknown))

    if timing["grid_bands"] <= 0:
        fail("grid_bands must be positive (%r given)" % (timing["grid_bands"],))
    if cfg["grid_step_m"] <= 0:
        fail("grid_step_m must be positive (%r given)" % (cfg["grid_step_m"],))
    if timing["mesh_cells"] <= 0 or timing["mesh_cells"] % 2:
        fail("mesh_cells must be positive and even (%r given) - the middle "
             "line runs through the origin" % (timing["mesh_cells"],))
    validate_floor(cfg)
    if timing["title_wave_period_ms"] <= 0:
        fail("title_wave_period_ms must be positive (%r given) - "
             "boot_anim_title_wave() divides by it every frame" %
             (timing["title_wave_period_ms"],))
    if timing["grid_start_ms"] > timing["total_ms"]:
        fail("grid_start_ms (%d) must not be after total_ms (%d) - "
             "boot_anim_grid_climb() subtracts them as unsigned, so the "
             "grid would never climb at all" %
             (timing["grid_start_ms"], timing["total_ms"]))
    if timing["far_grow_ms"] <= 0:
        fail("far_grow_ms must be positive (%r given)" % (timing["far_grow_ms"],))
    if timing["title_start_ms"] < timing["grid_start_ms"] + timing["grid_bands"] * timing["grid_band_ms"] \
            + timing["far_grow_ms"]:
        fail("title_start_ms must not come before the floor reaches far_m - "
             "draw_far_grid() lifts the far lines only on the whole floor")
    if not 0 <= timing["title_shadow_alpha"] <= 255:
        fail("title_shadow_alpha must be 0..255 (%r given) - it is written "
             "straight into a #define read as a C uint8_t; a value outside "
             "that range would silently wrap rather than fail loudly on "
             "the device" % (timing["title_shadow_alpha"],))
    # 368/8/5 mirror BOOT_ANIM_TITLE_VIEW_H/the glyph cell in boot_anim.h,
    # the same "named here, not pulled from the C header" convention
    # suite_boot_anim.c's own top comment already uses for this panel's
    # fixed dimensions. Only a COARSE bound: title_amplitude_px/
    # title_wave_amplitude_px are the two knobs that can carry a landed
    # letter off title_height_px, so they stand in as a margin rather than
    # replaying ease_out_u8()'s own curve here; the exact simulation
    # is test_the_title_stays_on_the_panel_once_visible() in
    # suite_boot_anim.c, which this only backstops for a value obviously
    # wrong enough that no test run would ever be needed to see it.
    _title_view_h = PANEL_WIDTH
    _title_cell_h = 8 * 5
    _title_margin = (timing["title_amplitude_px"] +
                     timing["title_wave_amplitude_px"])
    if (timing["title_height_px"] - _title_margin < 0 or
            timing["title_height_px"] + _title_cell_h + _title_margin >
            _title_view_h):
        fail("title_height_px=%r would carry the title off the top or "
             "bottom of the %d-pixel-tall viewer's frame once its own "
             "wobble (title_amplitude_px=%r) and idle wave "
             "(title_wave_amplitude_px=%r) are accounted for" %
             (timing["title_height_px"], _title_view_h,
              timing["title_amplitude_px"], timing["title_wave_amplitude_px"]))
    # Exact, not coarse like title_height_px's own bound above: unlike Y,
    # X has no wobble or idle wave added to it once a letter lands (see
    # boot_anim_title_letter() in boot_anim.h, only p.y gets one) so
    # there is no margin to stand in for here, just the word's own fixed
    # width. 448/8/5/3/6 mirror BOOT_ANIM_TITLE_VIEW_W/the glyph cell/gap/
    # BOOT_ANIM_TITLE_LEN in boot_anim.h, the same "named here" convention
    # as _title_view_h above. No trailing gap after the LAST glyph, which
    # is why this is LEN cells minus one gap, not LEN cells outright; see
    # boot_anim_title_letter() in boot_anim.h for the same subtraction.
    _title_view_w = PANEL_HEIGHT
    _title_cell_w = 8 * 5 + 3
    _title_word_w = 6 * _title_cell_w - 3
    if (timing["title_x_px"] < 0 or
            timing["title_x_px"] + _title_word_w > _title_view_w):
        fail("title_x_px=%r would carry the title off the left or right "
             "of the %d-pixel-wide viewer's frame (the word itself is "
             "%d px wide at BOOT_ANIM_TITLE_SCALE)" %
             (timing["title_x_px"], _title_view_w, _title_word_w))
    if timing["image_start_ms"] < 0 or timing["image_fade_ms"] < 0:
        fail("image_start_ms/image_fade_ms must not be negative (%r/%r "
             "given) - boot_anim_photo_reveal() hands both to ease_ramp_u8() "
             "as a uint32_t, where a negative duration wraps to an "
             "enormous one and the crossfade would silently never finish" %
             (timing["image_start_ms"], timing["image_fade_ms"]))

    if timing["title_scale"] < 1:
        fail("title_scale must be at least 1 (%r given) - draw_text_font() "
             "clamps anything below that to 1 anyway, so a 0 here reads as "
             "an authored intent the renderer silently ignores" %
             (timing["title_scale"],))

    if timing["title_scale"] < 3:
        warn("title_scale %d gives a %dpx title - the 8x8 bitmap needs about 5x to read as a title" %
             (timing["title_scale"], 8 * timing["title_scale"]))

    if timing["title_wave_out_ms"] < 0 or timing["title_wave_fade_ms"] < 0:
        fail("title_wave_out_ms/title_wave_fade_ms must not be negative "
             "(%r/%r given) - boot_anim_title_wave_reach() hands both to "
             "ease_ramp_u8() as a uint32_t, the same wrap-to-enormous trap "
             "image_start_ms above describes" %
             (timing["title_wave_out_ms"], timing["title_wave_fade_ms"]))

    # Only worth saying when the calming is actually authored to happen at
    # all; the default parks title_wave_out_ms at total_ms, where the wave
    # simply never calms and there is nothing to warn about. Mirrors the
    # image_start_ms < total_ms guard on its own crossfade warning below.
    if (timing["title_wave_out_ms"] < timing["total_ms"] and
            timing["title_wave_out_ms"] + timing["title_wave_fade_ms"] >
            timing["total_ms"]):
        warn("the title's idle wave is still calming at total_ms (%d + %d > "
             "%d) - it will be cut off mid-settle rather than reaching "
             "stillness on screen"
             % (timing["title_wave_out_ms"], timing["title_wave_fade_ms"],
                timing["total_ms"]))

    pen_end = timing["pen_start_ms"] + timing["pen_ms"]
    if pen_end > timing["fade_start_ms"]:
        fail("pen_start_ms + pen_ms (%d) must not exceed fade_start_ms (%d) "
             "- the curve would still be drawing when the picture starts "
             "dissolving" % (pen_end, timing["fade_start_ms"]))

    if timing["pen_finish_ms"] <= pen_end:
        fail("pen_finish_ms (%d) must be after pen_start_ms + pen_ms (%d) - "
             "phase 2 needs positive duration to actually finish drawing "
             "the curve in" % (timing["pen_finish_ms"], pen_end))

    if timing["fade_start_ms"] >= timing["total_ms"]:
        fail("fade_start_ms (%d) must be before total_ms (%d) - there would "
             "be no time left to dissolve" %
             (timing["fade_start_ms"], timing["total_ms"]))

    if timing["image_start_ms"] < timing["total_ms"]:
        image_done = timing["image_start_ms"] + timing["image_fade_ms"]
        if image_done > timing["fade_start_ms"]:
            warn("the photograph finishes crossing in at %d, after "
                 "fade_start_ms (%d) - it will still be arriving as the "
                 "picture starts dissolving"
                 % (image_done, timing["fade_start_ms"]))

    if timing["pen_finish_ms"] > timing["fade_start_ms"]:
        warn("pen_finish_ms (%d) is after fade_start_ms (%d) - the curve "
             "will still be drawing when the picture starts dissolving"
             % (timing["pen_finish_ms"], timing["fade_start_ms"]))



TIMING_ORDER = [
    ("total_ms", "BOOT_ANIM_MS",
     "how long the whole animation runs, start to black"),
    ("axes_ms", "BOOT_ANIM_AXES_MS",
     "the axes grow out of the origin"),
    ("grid_start_ms", "BOOT_ANIM_GRID_START_MS", None),
    ("grid_band_ms", "BOOT_ANIM_GRID_BAND_MS",
     "each band waits for the one inside"),
    ("grid_bands", "BOOT_ANIM_GRID_BANDS",
     "how many grid_step_m bands out the floor's colour and fade reach"),
    ("mesh_cells", "BOOT_ANIM_MESH_CELLS",
     "cells along each side of the floor's square mesh - even, so one line "
     "runs through the origin"),
    ("far_grow_ms", "BOOT_ANIM_FAR_GROW_MS",
     "how long the floor takes to grow from the mesh's edge to far_m once "
     "the mesh has arrived"),
    ("grid_fade_ms", "BOOT_ANIM_GRID_FADE_MS", None),
    ("ripple_impact_ms", "BOOT_ANIM_RIPPLE_IMPACT_MS", None),
    ("ripple_calm_after_ms", "BOOT_ANIM_RIPPLE_CALM_AFTER_MS", None),
    ("mesh_dash_on_px", "BOOT_ANIM_MESH_DASH_ON_PX", None),
    ("mesh_dash_off_px", "BOOT_ANIM_MESH_DASH_OFF_PX", None),
    ("swell_in_ms", "BOOT_ANIM_SWELL_IN_MS", None),
    ("pen_start_ms", "BOOT_ANIM_PEN_START_MS", None),
    ("pen_ms", "BOOT_ANIM_PEN_MS", "how long the curve takes to draw"),
    ("pen_finish_ms", "BOOT_ANIM_PEN_FINISH_MS",
     "when the curve reaches the table's true end - independent of "
     "fade_start_ms, so it can keep drawing well past phase 1"),
    ("title_start_ms", "BOOT_ANIM_TITLE_START_MS",
     "the title arrives after the curve, not during it"),
    ("title_stagger_ms", "BOOT_ANIM_TITLE_STAGGER_MS",
     "each letter starts this much after the one before it"),
    ("title_flight_ms", "BOOT_ANIM_TITLE_FLIGHT_MS",
     "how long ONE letter's flight is"),
    ("title_entry_px", "BOOT_ANIM_TITLE_ENTRY_PX",
     "how far off-panel a letter starts"),
    ("title_turns_phase", "BOOT_ANIM_TITLE_TURNS_PHASE",
     "wobble oscillations packed into the early part of the flight"),
    ("title_amplitude_px", "BOOT_ANIM_TITLE_AMPLITUDE_PX",
     "peak wobble swing right at the start"),
    ("title_wave_amplitude_px", "BOOT_ANIM_TITLE_WAVE_AMPLITUDE_PX",
     "the small idle wave once a letter has landed - keep under 22 px or it hurts legibility"),
    ("title_wave_period_ms", "BOOT_ANIM_TITLE_WAVE_PERIOD_MS", None),
    ("title_wave_stagger_ms", "BOOT_ANIM_TITLE_WAVE_STAGGER_MS", None),
    ("title_scale", "BOOT_ANIM_TITLE_SCALE",
     "integer pixel multiplier for the 8x8 title font"),
    ("title_wave_out_ms", "BOOT_ANIM_TITLE_WAVE_OUT_MS",
     "when the idle wave starts calming back to stillness - at or past "
     "total_ms it never does, which is the default"),
    ("title_wave_fade_ms", "BOOT_ANIM_TITLE_WAVE_FADE_MS",
     "how long that calming takes, from full swing to none"),
    ("title_height_px", "BOOT_ANIM_TITLE_VIEW_Y",
     "the title's centre in the viewer's frame; see the comment above "
     "BOOT_ANIM_TITLE_VIEW_W in boot_anim.h"),
    ("title_x_px", "BOOT_ANIM_TITLE_VIEW_X",
     "how far into the viewer's frame the title's own left edge starts"),
    ("title_shadow_dx", "BOOT_ANIM_TITLE_SHADOW_DX",
     "drop shadow offset, pixels right (negative is left) - 0/0 disables it"),
    ("title_shadow_dy", "BOOT_ANIM_TITLE_SHADOW_DY",
     "drop shadow offset, pixels down (negative is up)"),
    ("title_shadow_alpha", "BOOT_ANIM_TITLE_SHADOW_ALPHA",
     "shadow density, 0..255 - 0 invisible, 255 solid, between is a "
     "dithered fake transparency (see draw_rect_dither() in draw.c)"),
    ("image_start_ms", "BOOT_ANIM_IMAGE_START_MS",
     "the photograph starts crossing in as the function crosses out"),
    ("image_fade_ms", "BOOT_ANIM_IMAGE_FADE_MS",
     "how long that one shared crossfade takes - 0 cuts straight to the photo"),
    ("fade_start_ms", "BOOT_ANIM_FADE_START_MS",
     "dissolve into the launcher"),
]

def main():
    if len(sys.argv) not in (2, 3):
        fail("usage: gen_boot_anim_timeline.py <boot_anim_timeline.json> [<boot_anim_motion.glb>]")

    with open(sys.argv[1], "r", encoding="utf-8") as f:
        cfg = json.load(f)
    if "camera_focal" in cfg:
        fail("unknown key: camera_focal")
    if "camera_half_fov_short_tan" not in cfg:
        fail("missing camera_half_fov_short_tan")
    if not math.isfinite(float(cfg["camera_half_fov_short_tan"])):
        fail("camera_half_fov_short_tan must be finite")
    if float(cfg["camera_half_fov_short_tan"]) <= 0:
        fail("camera_half_fov_short_tan must be greater than 0")
    if "keyframes" in cfg:
        fail("keyframes no longer live in the timeline: the camera and space "
             "move by main/boot/boot_anim_motion.glb")

    # pen_finish_ms is newer than fade_start_ms; a file baked before it
    # existed has no way to carry it. Defaulting it to fade_start_ms
    # reproduces exactly what boot_anim_pen() always did before the two
    # were split apart (see its own "TWO PHASES" comment), so an old
    # timeline keeps behaving the way it always did rather than failing to
    # load at all over a field its author never had reason to set.
    timing = cfg.get("timing", {})
    timing.setdefault("pen_finish_ms", timing.get("fade_start_ms"))

    # image_start_ms/image_fade_ms are newer again; a file baked before
    # they existed had no photograph in it at all, and total_ms is what
    # reproduces that exactly: boot_anim_run() never draws a frame past
    # total_ms, so defaulting image_start_ms TO total_ms means boot_anim_
    # image_reveal() is 0 for every frame that is ever actually drawn;
    # the crossfade is authored but never reached, not merely set to a
    # short/instant duration, until someone deliberately pulls it
    # earlier. image_fade_ms's own default only matters once that happens.
    # title_height_px is newer than the title itself; BOOT_ANIM_TITLE_
    # VIEW_Y used to be a fixed constant in boot_anim.h (derived from a
    # golden-rectangle construction, see that section's own comment,
    # still there as this default's own reasoning), so 50, what it was
    # fixed at, is what keeps a file baked before this existed landing
    # the title exactly where it always did.
    timing.setdefault("title_height_px", 50)
    # title_x_px is newer still; BOOT_ANIM_TITLE_VIEW_X was the last of
    # the pair still fixed in boot_anim.h, same golden-rectangle
    # derivation as Y's own default above, hand-nudged from there for
    # margin (see that section's own comment for the full chain), 170,
    # what it was fixed at, for the same "a file baked before this
    # existed keeps landing the title exactly where it always did" reason.
    timing.setdefault("title_x_px", 170)
    # title_shadow_dx/dy are newer again; the shadow used to be a fixed
    # (1, 1) two draw_text_font() calls always drew, so 1/1 is what keeps
    # a file baked before this existed looking exactly the same rather
    # than silently losing its shadow.
    timing.setdefault("title_shadow_dx", 1)
    timing.setdefault("title_shadow_dy", 1)
    # title_shadow_alpha is newer again; the shadow used to be fully
    # solid (there was no dithering to be less than that), so 255 is
    # what keeps a file baked before this existed looking exactly the
    # same rather than silently turning translucent.
    timing.setdefault("title_shadow_alpha", 255)
    timing.setdefault("image_start_ms", timing.get("total_ms", 5800))
    timing.setdefault("image_fade_ms", 800)
    # title_wave_out_ms/title_wave_fade_ms are newer again; the idle wave
    # used to run at full swing for as long as the title was on screen,
    # with no way to settle it. Defaulting the START of the calming to
    # total_ms means boot_anim_title_wave_reach() is 255 for every frame
    # boot_anim_run() actually draws (it stops at total_ms), so a file
    # baked before these existed keeps its wave running exactly as it
    # did: the same "default it past the end so it never fires" trick
    # image_start_ms above already uses.
    timing.setdefault("title_wave_out_ms", timing.get("total_ms", 5800))
    timing.setdefault("title_wave_fade_ms", 600)
    timing.setdefault("title_scale", 5)

    validate(cfg)
    if len(sys.argv) == 3:
        validate_motion(load_motion(sys.argv[2]), cfg["timing"])

    # See gen_zeta_curve.py's own comment on this; LF only, so regenerating
    # on Windows does not rewrite the file for everyone else.
    sys.stdout.reconfigure(newline="\n")

    timing = cfg["timing"]

    w = sys.stdout.write
    w("/*=============================================================================\n")
    w(" * GENERATED FILE - do not edit.\n")
    w(" *\n")
    w(" *     python tools/gen/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json main/boot/boot_anim_motion.glb > main/boot/boot_anim_timeline.h\n")
    w(" *\n")
    w(" * The boot animation's timing constants, settings and floor tables, from\n")
    w(" * main/boot/boot_anim_timeline.json and turned into this header by\n")
    w(" * this script. Its camera and space move by the boot clip,\n")
    w(" * main/boot/boot_anim_motion.anim.toml, instead.\n")
    w(" *===========================================================================*/\n")
    w("#pragma once\n\n#include <stdint.h>\n\n")

    for key, name, note in TIMING_ORDER:
        if note:
            w("/* %s */\n" % note)
        w("#define %s %d\n" % (name, timing[key]))
        w("\n" if note else "")

    w("/* Positive tangent of the camera's shorter-axis half field of view. */\n")
    w("#define BOOT_ANIM_CAMERA_HALF_FOV_SHORT_TAN %sF\n\n" % repr(float(cfg["camera_half_fov_short_tan"])))

    w("/* The side of one floor mesh cell: the mesh reaches exactly as far as\n")
    w(" * the floor's fade, grid_bands bands of grid_step_m, in mesh_cells / 2\n")
    w(" * cells either side of the origin. */\n")
    w("#define BOOT_ANIM_MESH_STEP_Q12 %d\n\n" % mesh_step_q12(cfg))

    w("/* The floor's colour band spacing - see BOOT_ANIM_GRID_BANDS's own comment\n")
    w(" * in boot_anim.h. Authored in meters (grid_step_m in the JSON), like\n")
    w(" * a transform's own pos, and converted here the same way units() in\n")
    w(" * boot_anim.c does. */\n")
    w("#define BOOT_ANIM_GRID_STEP_Q12 %d\n\n" % meters_to_q12(cfg["grid_step_m"]))

    emit_floor(cfg, w)
    emit_crest(cfg, w)
    emit_axis_far(cfg, w)
    emit_far_grid(cfg, w)


if __name__ == "__main__":
    main()
