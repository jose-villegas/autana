"""Score the fit on its held-out reference sets with its own rasterizer."""
import re

from r3d import appearance_simplify as appearance
from r3d.fitted_variant import BOARD_SCALE
from r3d.reference_render import render_sets
from r3d.reference_set import HELD_OUT, PROBE_HELD_OUT, reference_inputs, unpack_references

# Parses appearance.held_out_line pending a separate held_out_mean at the next fit re-key.
SCORE = re.compile(r"^held-out dE76 \(fit rasteriser\): (?P<mean>\d+\.\d+) over (?P<views>\d+) views$")
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


def held_out_sets(fit, cache, work):
    """The same held-out pose sets the fit stage passes to the rasterizer."""
    from bake import bake, produce

    inputs = reference_inputs(fit.job, fit.scene)
    sets = inputs.sets().held_out
    if not sets:
        return []
    cached = produce.reference_folder(cache, fit.stages["reference"])
    if (cached / produce.DONE).is_file():
        produce.read_references(cached)
        work = cached
    else:
        work.mkdir(parents=True, exist_ok=True)
        unpack_references(bake.fetch(fit.reference, cache).read_bytes(), work)
        missing = [(work / poses, work / reference) for poses, reference in sets
                   if not appearance.pictured(work / reference)]
        if missing:
            render_sets(inputs, missing, store=produce.PoseStore(cache, fit.stages["reference_pose"]))
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
