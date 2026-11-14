"""Prepare source reference sets and the inputs their keys describe."""

import dataclasses
import io
import pathlib
import tarfile
from types import SimpleNamespace

POSE_SPACING = ("train_every_ms", "held_out_every_ms", "coverage_every_ms")
# A reference set's files, by role: each (poses, references) pair the fit trains on, the poses its prune counts
# shown pixels over, and the held-out pair that scores a fit; the camera path's, then the probes' (r3d/fit/probes.py).
TRAINING = (("train.txt", "reference"), ("train_landscape.txt", "reference_landscape"))
COVERAGE = "coverage.txt"
HELD_OUT = ("held_out.txt", "reference_held_out")
PROBE_TRAINING = ("probes.txt", "reference_probes")
PROBE_HELD_OUT = ("probes_held_out.txt", "reference_probes_held_out")
PROBE_RECORD = "probes.json"
# Probe placement is random; one fixed seed keeps a reference set's key its content.
PROBE_SEED = 1
# What a packed set (pack_references) holds of a reference: the picture the fit compares and its normals.
PACKED_SUFFIXES = (".png", ".normal.npy")


def fit_sets(path, probed, shaped):
    """The sets of a camera with a `path` and an import with geometry.probes (`probed`), one that shapes its fit
    (`shaped`: probes or geometry.budget): `training` pairs, `coverage` poses, `held_out` pairs, and `scored`,
    the held-out pairs the fit reads to count holes, none unless shaped. The prune keeps what the path shows
    before what only a probe does, so the path's sets come first."""
    held_out = ((HELD_OUT,) if path else ()) + ((PROBE_HELD_OUT,) if probed else ())
    return SimpleNamespace(training=(TRAINING if path else ()) + ((PROBE_TRAINING,) if probed else ()),
                           coverage=((COVERAGE,) if path else ()) + ((PROBE_TRAINING[0],) if probed else ()),
                           held_out=held_out, scored=held_out if shaped else ())


def shapes_fit(fit):
    """Whether an import's `fit` (geometry.probes, geometry.budget) shapes its fit."""
    return fit.probes is not None or fit.budget is not None


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
    import, the scene's bake light, tone map and camera, the size and spacing of the poses, the import's
    probes (None without geometry.probes) and whether it shapes the fit. Nothing of
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
    probes: SimpleNamespace = None  # the import's geometry.probes
    shaped: bool = False          # the import shapes the fit (shapes_fit): its pack holds the scored references

    def sets(self):
        return fit_sets(self.camera.path is not None, self.probes is not None, self.shaped)

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
                           poses={name: getattr(renderer.fit, name) for name in POSE_SPACING},
                           probes=job.settings.fit.probes, shaped=shapes_fit(job.settings.fit))


def prepare_references(inputs, work):
    """The reference set of `inputs` into `work`: the training, held-out and pruning poses sampled from the
    camera path, the probes' training and held-out faces when the import sets probes (a camera without a path
    has only those) and the references of every training and held-out set with their normals.
    Returns the training views."""
    from r3d.mesh_source import camera_path_poses
    from r3d.reference_render import render_sets

    scene, spacing = inputs.scene(), SimpleNamespace(**inputs.poses)
    camera = scene.camera.component
    work.mkdir(parents=True, exist_ok=True)
    path, count = None, 0
    if camera.path is not None:
        visibility = SimpleNamespace(size=inputs.size, every_ms=None)
        w, h, lens, near, poses = camera_path_poses(scene, visibility, spacing.train_every_ms, either_way_up=False)
        training, held_out = split_poses(spacing, poses)
        (portrait, _), (landscape, _) = TRAINING
        (work / portrait).write_text(poses_text(w, h, lens, near, training))
        (work / landscape).write_text(poses_text(h, w, lens, near, training))
        (work / HELD_OUT[0]).write_text(poses_text(w, h, lens, near, held_out))
        coverage = camera_path_poses(scene, visibility, spacing.coverage_every_ms)
        (work / COVERAGE).write_text(poses_text(*coverage))
        path, count = (coverage[0], coverage[1], coverage[2], coverage[4]), len(training)
    if inputs.probes is not None:
        count += probe_poses(inputs, work, path)
    sets = inputs.sets()
    render_sets(inputs, [(work / poses, work / reference) for poses, reference in sets.training + sets.held_out])
    return count


def probe_poses(inputs, work, path):
    """Writes the probes' training and held-out faces (r3d/fit/probes.py) and their record; returns the
    training faces."""
    import json
    from r3d.fit import probes
    from r3d.reference_render import lit_source

    camera = inputs.scene().camera.component
    face = probes.PROBE_FACE if inputs.probes.face is None else inputs.probes.face
    source = lit_source(inputs.job(), inputs.scene(), lit=False)
    training, held_out, record = probes.select(source.p, source.tri_v, camera.region, camera.near_z,
                                               inputs.probes.spacing, PROBE_SEED, path)
    for (poses, _), faces in ((PROBE_TRAINING, training), (PROBE_HELD_OUT, held_out)):
        (work / poses).write_text(poses_text(face, face, probes.FACE_LENS, camera.near_z, faces))
    (work / PROBE_RECORD).write_text(json.dumps(record) + "\n")
    return len(training)


def pack_references(work, sets):
    """The files of the reference set in `work` that a fit with `sets` (fit_sets) reads, as one .tar.xz: the pose
    files and the picture and normals of each training and scored reference. The other held-out references are
    left out: they score a fit, not make it. Names are sorted and carry no time or owner, so the same files pack
    to the same bytes."""
    work = pathlib.Path(work)
    poses = {poses for poses, _ in sets.training + sets.held_out} | set(sets.coverage)
    names = sorted(poses) + sorted(
        f"{folder}/{path.name}" for _, folder in sets.training + sets.scored for path in (work / folder).iterdir()
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
