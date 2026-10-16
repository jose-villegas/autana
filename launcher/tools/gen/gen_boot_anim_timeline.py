#!/usr/bin/env python3
"""Generate main/boot/boot_anim_timeline.h: the boot animation's timing
constants, scalar settings and baked floor tables.

    python tools/gen/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json main/boot/boot_anim_motion.glb > main/boot/boot_anim_timeline.h

WHERE THIS DATA COMES FROM

main/boot/boot_anim_timeline.json is the source of truth for everything
that is not motion, edited by hand. The camera and the space the grid and
curve live in move by a glTF animation instead,
main/boot/boot_anim_motion.glb (see docs/Animation-Tracks.md), authored in
any glTF tool and baked into the boot clip's own asset pack by
tools/r3d/build_pack.py from main/boot/boot_anim_motion.anim.toml; this
script only reads it to check it.

The output header includes the floor's distance bands, attenuation, ripple
shape, decay and swell phases. The mesh geometry and its radial bounds are
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
FAR_SEGMENT_LIMIT = 16
FAR_BAND_LIMIT = 1 << 16
FAR_REACH_LIMIT_M = 500
FAR_SPACING_GROWTH_MIN = 2
FAR_SPACING_GROWTH_MAX = 8


def floor_number(value, name, minimum=0, positive=False, integer=False):
    if (not isinstance(value, (float, int)) or isinstance(value, bool) or
            not math.isfinite(value) or (minimum is not None and value < minimum) or
            (positive and value == minimum) or (integer and int(value) != value)):
        fail(name + " must be " + ("positive " if positive else "nonnegative " if minimum is not None else "") +
             ("finite integer" if integer else "finite number"))


def validate_floor(cfg):
    allowed = {"timing", "camera_half_fov_short_tan", "grid_step_m",
               "ripple_wavelength_m", "ripple_speed_m_s", "ripple_rings",
               "ripple_height_m", "swell_height_m", "swell", "far_line_stride", "far_reach_m",
               "far_segments", "far_spacing_growth", "swell_flat_start_m"}
    for key in sorted(set(cfg) - allowed):
        fail("unknown key: " + key)
    for key in ("ripple_wavelength_m", "ripple_speed_m_s", "ripple_rings"):
        floor_number(cfg.get(key), key, positive=True)
    for key in ("ripple_height_m", "swell_height_m"):
        floor_number(cfg.get(key), key)
    for key in ("ripple_impact_ms", "ripple_decay_ms", "swell_start_ms", "swell_in_ms"):
        floor_number(cfg["timing"].get(key), key,
                     positive=key in ("ripple_decay_ms", "swell_in_ms"), integer=True)
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
    sigma = cfg["ripple_rings"] * cfg["ripple_wavelength_m"]
    extent = sigma * math.sqrt(math.log(max(1, cfg["ripple_height_m"] * BOOT_ANIM_ONE * 2))) * FLOOR_SHAPE_SCALE
    if not math.isfinite(extent) or extent > (1 << 15) - 1:
        fail("ripple_rings * ripple_wavelength_m must fit the signed shape index")
    travel = cfg["ripple_speed_m_s"] * (cfg["timing"]["total_ms"] + (2 * FLOOR_TIME_STEP_MS)) * FLOOR_SHAPE_SCALE / 1000
    if not math.isfinite(travel) or travel > (1 << 15) - 1:
        fail("ripple_speed_m_s must fit the signed travel index")
    reach = math.sqrt(2) * mesh_step_q12(cfg) * (cfg["timing"]["mesh_cells"] // 2)
    if reach * FLOOR_SHAPE_SCALE / BOOT_ANIM_ONE >= (1 << 16):
        fail("grid_step_m * grid_bands must fit the unsigned radius index")
    validate_far_floor(cfg)


def validate_far_floor(cfg):
    reach = mesh_step_q12(cfg) * (cfg["timing"]["mesh_cells"] // 2) / BOOT_ANIM_ONE
    for key in ("far_line_stride", "far_segments", "far_spacing_growth"):
        floor_number(cfg.get(key), key, positive=True, integer=True)
    if cfg["far_line_stride"] > cfg["timing"]["mesh_cells"] // 2:
        fail("far_line_stride must not exceed half mesh_cells")
    if cfg["far_segments"] > FAR_SEGMENT_LIMIT:
        fail("far_segments must not exceed %d" % FAR_SEGMENT_LIMIT)
    if not FAR_SPACING_GROWTH_MIN <= cfg["far_spacing_growth"] <= FAR_SPACING_GROWTH_MAX:
        fail("far_spacing_growth must be between %d and %d" % (FAR_SPACING_GROWTH_MIN, FAR_SPACING_GROWTH_MAX))
    floor_number(cfg.get("far_reach_m"), "far_reach_m", positive=True)
    if cfg["far_reach_m"] <= reach or cfg["far_reach_m"] > FAR_REACH_LIMIT_M:
        fail("far_reach_m must exceed the mesh reach and not exceed %d metres" % FAR_REACH_LIMIT_M)
    band = meters_to_q12(cfg["grid_step_m"])
    if band <= 0:
        fail("grid_step_m must encode a positive Q12 band")
    if math.sqrt(2) * meters_to_q12(cfg["far_reach_m"]) / band >= FAR_BAND_LIMIT:
        fail("far_reach_m must fit unsigned distance bands at grid_step_m")
    floor_number(cfg.get("swell_flat_start_m"), "swell_flat_start_m")
    if cfg["swell_flat_start_m"] >= reach:
        fail("swell_flat_start_m must be inside the mesh reach")


def emit_far_floor(cfg, w):
    half = cfg["timing"]["mesh_cells"] // 2
    step = mesh_step_q12(cfg)
    near = half * step
    far = meters_to_q12(cfg["far_reach_m"])
    count = cfg["far_segments"]
    distances = [round(near * (far / near)**(i / count)) for i in range(count + 1)]
    offsets = list(range(0, half + 1, cfg["far_line_stride"]))
    stride = cfg["far_line_stride"]
    offset = offsets[-1] + stride
    boundary = half * cfg["far_spacing_growth"]
    while offset * step <= far:
        offsets.append(offset)
        if offset >= boundary:
            stride *= cfg["far_spacing_growth"]
            boundary *= cfg["far_spacing_growth"]
        offset += stride
    last = (far // (cfg["far_line_stride"] * step)) * cfg["far_line_stride"]
    if offsets[-1] != last:
        offsets.append(last)
    w("#define BOOT_ANIM_FAR_SEGMENTS %d\n" % count)
    w("#define BOOT_ANIM_FAR_REACH_Q12 %d\n" % far)
    w("#define BOOT_ANIM_FAR_LINE_STRIDE %d\n" % cfg["far_line_stride"])
    w("#define BOOT_ANIM_FAR_SPACING_GROWTH %d\n" % cfg["far_spacing_growth"])
    w("#define BOOT_ANIM_SWELL_FLAT_START_M %s\n" % repr(float(cfg["swell_flat_start_m"])))
    w("#define BOOT_ANIM_GRID_FADE %d\n" % (far // meters_to_q12(cfg["grid_step_m"])))
    w("static const int32_t boot_anim_far_distances[] = {" + ", ".join(map(str, distances)) + "};\n")
    w("typedef struct { int32_t offset; uint16_t band[BOOT_ANIM_FAR_SEGMENTS + 1]; } boot_anim_far_line_t;\n")
    w("static const boot_anim_far_line_t boot_anim_far_lines[] = {\n")
    band = meters_to_q12(cfg["grid_step_m"])
    for offset in offsets:
        x = offset * step
        midpoints = [0] + [(a + b) // 2 for a, b in zip(distances, distances[1:])]
        bands = [math.isqrt((x * x) + (y * y)) // band for y in midpoints]
        w("    {%d, {%s}},\n" % (x, ", ".join(map(str, bands))))
    w("};\n")


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
    sigma = cfg["ripple_rings"] * cfg["ripple_wavelength_m"]
    extent = math.ceil(sigma * math.sqrt(math.log(max(1, cfg["ripple_height_m"] * BOOT_ANIM_ONE * 2))) * FLOOR_SHAPE_SCALE)
    for key in ("ripple_wavelength_m", "ripple_speed_m_s", "ripple_rings", "ripple_height_m", "swell_height_m"):
        w("#define BOOT_ANIM_%s %s\n" % (key.upper(), repr(float(cfg[key]))))
    w("#ifndef DEVICE_BUILD\n")
    w("typedef struct { double wavelength_m, period_ms, direction_deg, weight; } boot_anim_swell_setting_t;\n")
    w("static const boot_anim_swell_setting_t boot_anim_swell_settings[] = {\n")
    for sine in cfg["swell"]:
        w("    {" + ", ".join(repr(float(sine[key])) for key in ("wavelength_m", "period_ms", "direction_deg", "weight")) + "},\n")
    w("};\n#endif\n")
    w("#define BOOT_ANIM_FLOOR_BAND_SHIFT %d\n" % FLOOR_BAND_SHIFT)
    w("#define BOOT_ANIM_RIPPLE_SHAPE_SCALE %d\n" % FLOOR_SHAPE_SCALE)
    w("#define BOOT_ANIM_RIPPLE_SHAPE_EXTENT %d\n" % extent)
    w("#define BOOT_ANIM_FLOOR_TIME_STEP_MS %d\n" % FLOOR_TIME_STEP_MS)
    w("#define BOOT_ANIM_FLOOR_GAIN_SHIFT 15\n")
    w("#define BOOT_ANIM_SWELL_COUNT %d\n\n" % len(cfg["swell"]))
    w("typedef struct { uint16_t radius, band_q4, falloff, envelope; } boot_anim_floor_node_t;\n")
    rows = []
    for j in range(half + 1):
        for i in range(half + 1):
            r_q12 = math.isqrt((i * step)**2 + (j * step)**2)
            r = r_q12 / BOOT_ANIM_ONE
            rows.append((round(r * FLOOR_SHAPE_SCALE), (r_q12 << FLOOR_BAND_SHIFT) // band,
                         round(FLOOR_GAIN_ONE / math.sqrt(1 + r / cfg["ripple_wavelength_m"])),
                         round(FLOOR_GAIN_ONE * min(1, max(0, (half * step / BOOT_ANIM_ONE - r) /
                               (half * step / BOOT_ANIM_ONE - cfg["swell_flat_start_m"]))))))
    floor_array(w, "boot_anim_floor_node_t", "boot_anim_floor_nodes", rows)
    w("static const int16_t boot_anim_ripple_shape[] = {\n")
    values = []
    for n in range(-extent, extent + 1):
        x = n / FLOOR_SHAPE_SCALE
        values.append(meters_to_q12(cfg["ripple_height_m"] * math.exp(-(x / sigma)**2) * math.cos(math.tau * x / cfg["ripple_wavelength_m"])))
    for start in range(0, len(values), 16):
        w("    " + ", ".join(map(str, values[start:start + 16])) + ",\n")
    w("};\n\n")
    w("typedef struct { int16_t travel; uint16_t decay, swell; uint16_t phase[BOOT_ANIM_SWELL_COUNT]; } boot_anim_floor_time_t;\n")
    w("static const boot_anim_floor_time_t boot_anim_floor_times[] = {\n")
    for ms in range(0, timing["total_ms"] + FLOOR_TIME_STEP_MS * 2, FLOOR_TIME_STEP_MS):
        tau = max(0, ms - timing["ripple_impact_ms"])
        travel = round(cfg["ripple_speed_m_s"] * tau * FLOOR_SHAPE_SCALE / 1000)
        decay = round(FLOOR_GAIN_ONE * math.exp(-tau / timing["ripple_decay_ms"]))
        gain = round(FLOOR_GAIN_ONE * min(1, max(0, (ms - timing["swell_start_ms"]) / timing["swell_in_ms"])))
        phases = [round(-ms * FLOOR_PHASE_TURN / s["period_ms"]) % FLOOR_PHASE_TURN for s in cfg["swell"]]
        w("    {%d, %d, %d, {%s}},\n" % (travel, decay, gain, ", ".join(map(str, phases))))
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
    if timing["mesh_dash"] not in (0, 1):
        fail("mesh_dash must be 0 or 1 (%r given)" % (timing["mesh_dash"],))
    validate_floor(cfg)
    if timing["grid_hue_ms"] <= 0:
        fail("grid_hue_ms must be positive (%r given) - boot_anim_grid_hue() "
             "divides by it every frame" % (timing["grid_hue_ms"],))
    if timing["title_wave_period_ms"] <= 0:
        fail("title_wave_period_ms must be positive (%r given) - "
             "boot_anim_title_wave() divides by it every frame" %
             (timing["title_wave_period_ms"],))
    if timing["grid_start_ms"] > timing["total_ms"]:
        fail("grid_start_ms (%d) must not be after total_ms (%d) - "
             "boot_anim_grid_climb() subtracts them as unsigned, so the "
             "grid would never climb at all" %
             (timing["grid_start_ms"], timing["total_ms"]))
    if timing["grid_spokes"] < 0:
        fail("grid_spokes must not be negative (%r given) - 0 hides them, "
             "there is no such thing as fewer" % (timing["grid_spokes"],))
    if timing["grid_spoke_dash"] not in (0, 1):
        fail("grid_spoke_dash must be 0 or 1 (%r given)" %
             (timing["grid_spoke_dash"],))
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
    # replaying tween_ease_out()'s own curve here; the exact simulation
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
             "given) - boot_anim_photo_reveal() hands both to tween_ramp() "
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
             "tween_ramp() as a uint32_t, the same wrap-to-enormous trap "
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
    ("mesh_dash", "BOOT_ANIM_MESH_DASH",
     "draws the mesh lines as alternating dashes instead of solid - "
     "0 solid, 1 dashed"),
    ("grid_spokes", "BOOT_ANIM_GRID_SPOKES",
     "radial guide lines from the origin to the floor's own edge - 0 hides them"),
    ("grid_spoke_dash", "BOOT_ANIM_GRID_SPOKE_DASH",
     "draws each spoke as alternating dashes instead of a solid line - "
     "0 solid, 1 dashed"),
    ("grid_spoke_start_ms", "BOOT_ANIM_GRID_SPOKE_START_MS",
     "spokes start drawing outward from the origin"),
    ("grid_spoke_draw_ms", "BOOT_ANIM_GRID_SPOKE_DRAW_MS",
     "how long a spoke takes to reach the floor's own edge - 0 draws the "
     "full length instantly"),
    ("grid_fade_ms", "BOOT_ANIM_GRID_FADE_MS", None),
    ("ripple_impact_ms", "BOOT_ANIM_RIPPLE_IMPACT_MS", None),
    ("ripple_decay_ms", "BOOT_ANIM_RIPPLE_DECAY_MS", None),
    ("swell_start_ms", "BOOT_ANIM_SWELL_START_MS", None),
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
    ("grid_hue_ms", "BOOT_ANIM_GRID_HUE_MS",
     "one whole hue-wheel turn takes this long"),
    ("grid_hue_spread", "BOOT_ANIM_GRID_HUE_SPREAD",
     "wheel positions per ring outward"),
    ("grid_whiten_max", "BOOT_ANIM_GRID_WHITEN_MAX", None),
    ("grid_ceiling_max", "BOOT_ANIM_GRID_CEILING_MAX", None),
    ("grid_max", "BOOT_ANIM_GRID_MAX", None),
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

    # grid_spokes is newer than the floor grid itself: the radial guide
    # lines used to be a fixed 8, unauthored; 8 is the exact same default
    # for a file baked before this existed, so it keeps looking the way it
    # always did.
    timing.setdefault("grid_spokes", 8)
    # grid_spoke_dash is newer still; a file baked before it existed drew
    # every spoke solid, so 0 (solid) is what keeps it looking the same.
    timing.setdefault("grid_spoke_dash", 0)
    # grid_spoke_start_ms/grid_spoke_draw_ms are newer again; a file baked
    # before they existed drew every spoke at full length the instant it
    # was eligible to show at all, with no outward reveal of its own. 0/0
    # reproduces exactly that: tween_ramp()'s own "dur_ms of 0 jumps
    # straight to 255 the instant now_ms passes start_ms" (see tween.h)
    # means a spoke is already full length by the first frame it would
    # have appeared in before these existed.
    timing.setdefault("grid_spoke_start_ms", 0)
    timing.setdefault("grid_spoke_draw_ms", 0)
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
    emit_far_floor(cfg, w)


if __name__ == "__main__":
    main()
