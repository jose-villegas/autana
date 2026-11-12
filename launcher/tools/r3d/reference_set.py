"""Prepare source reference sets and the inputs their keys describe."""

import dataclasses
import io
import pathlib
import tarfile
from types import SimpleNamespace

POSE_SPACING = ("train_every_ms", "held_out_every_ms", "coverage_every_ms")
# A reference set's files: each (poses, references) pair the fit trains on, the poses its prune counts shown pixels
# over, and the held-out pair that scores a fit.
TRAINING = (("train.txt", "reference"), ("train_landscape.txt", "reference_landscape"))
COVERAGE = "coverage.txt"
HELD_OUT = ("held_out.txt", "reference_held_out")
# What a packed set (pack_references) holds of a reference: the picture the fit compares and its normals.
PACKED_SUFFIXES = (".png", ".normal.npy")


def poses_text(width, height, lens, near, poses):
    lines = [f"size {width} {height}", f"lens {lens!r} {near!r}"]
    lines += ["pose " + " ".join("%.9g" % value for value in pose) for pose in poses]
    return "\n".join(lines) + "\n"


def split_poses(fit, poses):
    """(training, held out): the poses sampled every train_every_ms, those at
    multiples of held_out_every_ms (time 0 aside) held out."""
    training, held_out = [], []
    for index, pose in enumerate(poses):
        time = index * fit.train_every_ms
        (held_out if time and time % fit.held_out_every_ms == 0 else training).append(pose)
    return training, held_out


def canonical(value):
    """A JSON-ready form of parsed settings, independent of parser layout.
    The digest hashes parser fields, so an explicit default differs from an omitted one; the stamp test catches it."""
    if isinstance(value, pathlib.Path):
        return None
    if isinstance(value, SimpleNamespace):
        return {name: canonical(item) for name, item in sorted(vars(value).items())
                if name != "seed_given" and not isinstance(item, pathlib.Path)}
    if isinstance(value, dict):
        return {name: canonical(item) for name, item in sorted(value.items())}
    if isinstance(value, (list, tuple)):
        return [canonical(item) for item in value]
    if isinstance(value, (set, frozenset)):
        return sorted(canonical(item) for item in value)
    return value


@dataclasses.dataclass(frozen=True)
class ReferenceInputs:
    """Everything a fit's reference set reads, and so everything its bake key counts (bake/bake.py): the
    import, the scene's bake light, tone map and camera, and the size and spacing of the poses. Nothing of
    the start or its renderer, so fits that differ only there share one set. The set is rendered from
    these fields alone (`job()`, `scene()`): one the render reads and this lacks fails there."""
    settings: SimpleNamespace     # the import
    bake: SimpleNamespace         # the renderer's [bake]: ray offset, occlusion, bounced light
    lights: list
    indirect: SimpleNamespace
    tonemap_white: float
    camera: SimpleNamespace       # the scene camera: lens, background and path
    size: tuple                   # the fit recipe's pose size
    poses: dict                   # the fit recipe's POSE_SPACING fields

    def job(self):
        return SimpleNamespace(settings=self.settings, bake=self.bake)

    def scene(self):
        return SimpleNamespace(lights=self.lights, indirect=self.indirect, tonemap_white=self.tonemap_white,
                               camera=SimpleNamespace(component=self.camera))


def reference_inputs(job, scene):
    """The reference inputs of a fitted renderer, with its recipe's pose size."""
    renderer = job.renderer
    size = tuple(renderer.fit.size)
    return ReferenceInputs(settings=job.settings, bake=job.bake, lights=scene.lights, indirect=scene.indirect,
                           tonemap_white=scene.tonemap_white, camera=scene.camera.component, size=size,
                           poses={name: getattr(renderer.fit, name) for name in POSE_SPACING})


def prepare_references(inputs, work):
    """The reference set of `inputs` into `work`: the training, held-out and pruning poses sampled from the
    camera path, and the training and held-out references with their normals."""
    from r3d.mesh_source import camera_path_poses
    from r3d.reference_render import render_sets

    scene, spacing = inputs.scene(), SimpleNamespace(**inputs.poses)
    visibility = SimpleNamespace(size=inputs.size, every_ms=None)
    work.mkdir(parents=True, exist_ok=True)
    w, h, lens, near, poses = camera_path_poses(scene, visibility, spacing.train_every_ms, either_way_up=False)
    training, held_out = split_poses(spacing, poses)
    (portrait, _), (landscape, _) = TRAINING
    (work / portrait).write_text(poses_text(w, h, lens, near, training))
    (work / HELD_OUT[0]).write_text(poses_text(w, h, lens, near, held_out))
    (work / landscape).write_text(poses_text(h, w, lens, near, training))
    (work / COVERAGE).write_text(poses_text(*camera_path_poses(scene, visibility, spacing.coverage_every_ms)))
    render_sets(inputs, [(work / poses, work / reference) for poses, reference in TRAINING + (HELD_OUT,)])
    return len(training)


def pack_references(work):
    """The files of the reference set in `work` that a fit reads, as one .tar.xz: the pose files and each training
    reference's picture and normals. The held-out references are left out: they score a fit, not make it. Names
    are sorted and carry no time or owner, so the same files pack to the same bytes."""
    work = pathlib.Path(work)
    names = sorted([poses for poses, _ in TRAINING] + [COVERAGE, HELD_OUT[0]]) + sorted(
        f"{folder}/{path.name}" for _, folder in TRAINING for path in (work / folder).iterdir()
        if path.name.endswith(PACKED_SUFFIXES))
    packed = io.BytesIO()
    with tarfile.open(fileobj=packed, mode="w:xz", format=tarfile.USTAR_FORMAT) as tar:
        for name in names:
            data = (work / name).read_bytes()
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), 0o644
            tar.addfile(info, io.BytesIO(data))
    return packed.getvalue()


def unpack_references(data, work):
    """A packed reference set (pack_references) into the folder `work`, laid out as prepare_references writes it."""
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as tar:
        tar.extractall(work, filter="data")
