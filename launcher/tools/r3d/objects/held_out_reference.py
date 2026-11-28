"""Score the fit on its held-out reference sets with its own rasterizer."""
import re
from types import SimpleNamespace

from r3d import appearance_simplify as appearance
from r3d.fitted_variant import BOARD_SCALE
from r3d.mesh_source import camera_path_poses
from r3d.reference_render import render_sets
from r3d.reference_set import (HELD_OUT, PROBE_HELD_OUT, poses_text, reference_inputs, split_poses,
                               unpack_references)

SCORE = re.compile(r"(?P<mean>[\d.]+) over (?P<views>\d+) views$")
SET_NAMES = {HELD_OUT: "path", PROBE_HELD_OUT: "probe"}
RGB_CHANNELS = 3
RGB8_MAX = 255


def fit_rasterizer_reason():
    """Why the fit's CUDA rasterizer is unavailable, or None."""
    try:
        import torch
        import nvdiffrast.torch  # noqa: F401
    except ImportError as error:
        return f"fit rasterizer dependencies unavailable ({error})"
    if not torch.cuda.is_available():
        return "CUDA unavailable for the fit rasterizer"
    return None


def prepare_path_reference(fit, inputs, cache, work):
    """Trace the held-out flight poses when the fit archive carries no images for them."""
    from bake import produce

    spacing = SimpleNamespace(**inputs.poses)
    view = SimpleNamespace(size=inputs.size, every_ms=None)
    width, height, lens, near, poses = camera_path_poses(inputs.scene(), view, spacing.train_every_ms,
                                                        either_way_up=False)
    _, held_out = split_poses(spacing, poses)
    poses_file, reference = (work / filename for filename in HELD_OUT)
    poses_file.write_text(poses_text(width, height, lens, near, held_out), encoding="utf-8")
    store = produce.PoseStore(cache, fit.stages["reference_pose"])
    render_sets(inputs, [(poses_file, reference)], store=store)


def held_out_sets(fit, cache, work):
    """The same held-out pose sets the fit stage passes to the rasterizer."""
    from bake import bake, produce

    inputs = reference_inputs(fit.job, fit.scene)
    sets = inputs.sets().held_out
    cached = produce.reference_folder(cache, fit.stages["reference"])
    if (cached / produce.DONE).is_file():
        produce.read_references(cached)
        work = cached
    else:
        work.mkdir(parents=True, exist_ok=True)
        if PROBE_HELD_OUT in sets:
            unpack_references(bake.fetch(fit.reference, cache).read_bytes(), work)
        if HELD_OUT in sets and not appearance.pictured(work / HELD_OUT[1]):
            prepare_path_reference(fit, inputs, cache, work)
    return [(SET_NAMES[pair], work / pair[0], work / pair[1]) for pair in sets]


def measure_held_out(fit, cache, work, mesh):
    """The fit's mean dE76, view count and set names, or why it could not run."""
    reason = fit_rasterizer_reason()
    if reason:
        return {"reason": reason}
    sets = held_out_sets(fit, cache, work)
    if not sets:
        return {"reason": "the fit declares no held-out views"}
    views = [appearance.load_views([(poses, reference)], BOARD_SCALE) for _, poses, reference in sets]
    clear = tuple(value / RGB8_MAX for value in fit.scene.camera.component.background.to_bytes(RGB_CHANNELS, "big"))
    line = appearance.held_out_line(mesh, views, clear)
    score = SCORE.search(line)
    if score is None:
        raise ValueError(f"unrecognized fit rasterizer score: {line}")
    return dict(mean=float(score["mean"]), views=int(score["views"]), sets=[name for name, _, _ in sets])
