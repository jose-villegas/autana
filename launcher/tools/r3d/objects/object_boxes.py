#!/usr/bin/env python3
"""Write a scene renderer's source objects as world boxes, for the scene viewer's boxes view.

    object_boxes.py SCENE.scene.toml --out FILE [--object NAME] [--tolerance X]

One line per source object, `shape copies lo.x lo.y lo.z hi.x hi.y hi.z`:
copies of one shape (match_instances) share `shape`, `copies` counts them,
and the renderer's placement is applied. --object defaults to the scene's first mesh renderer. FILE.key
records what FILE was made from (the scene, the import settings, the source
files and this tool's code); a run whose key matches leaves FILE as it is.
"""

import argparse
import hashlib
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from r3d.import_settings import load_scene, source_files  # noqa: E402
from r3d.mesh_import import load_source  # noqa: E402
import numpy as np  # noqa: E402

from r3d.objects.instances import MATCH_TOLERANCE, match_instances, placed_corners  # noqa: E402
from r3d.objects.objects_report import masked_objects  # noqa: E402

# A box has a corner per choice of low or high on each of three axes.
BOX_CORNERS = 2 ** 3
# The modules whose code decides the boxes.
CODE = [pathlib.Path(__file__).resolve().parent / name for name in ('object_boxes.py', 'instances.py',
                                                                      'objects_report.py')]


def made_from(scene_path, job, tolerance):
    """A digest of everything FILE depends on; the source files by size and time, as they are large."""
    digest = hashlib.sha256(f'{job.object.name} {tolerance!r}\n'.encode())
    for path in (scene_path, job.settings.path, *CODE):
        digest.update(pathlib.Path(path).read_bytes())
    for path in sorted(source_files(job.settings)):
        stat = pathlib.Path(path).stat()
        digest.update(f'{path} {stat.st_size} {stat.st_mtime_ns}\n'.encode())
    return digest.hexdigest()


def placed_boxes(shapes, job, min_copies=1):
    """Every copy's placed corners and its shape's index, for shapes with at least min_copies copies."""
    boxes, owners = [], []
    matrix, position = np.asarray(job.object.matrix), np.asarray(job.object.position)
    for index, shape in enumerate(shapes):
        if len(shape.instances) < min_copies:
            continue
        for instance in shape.instances:
            boxes.append(placed_corners(shape, instance, matrix, position))
            owners.append(index)
    return np.asarray(boxes).reshape(-1, BOX_CORNERS, 3), np.asarray(owners, dtype=int)


def box_lines(shapes, job):
    """Each copy's world box, by shape."""
    corners, owners = placed_boxes(shapes, job)
    return [' '.join([str(owner), str(len(shapes[owner].instances)), *(f'{value:.9g}' for value in (*box.min(axis=0), *box.max(axis=0)))])
            for box, owner in zip(corners, owners)]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('scene', type=pathlib.Path)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--object')
    parser.add_argument('--tolerance', type=float, default=MATCH_TOLERANCE)
    args = parser.parse_args(argv)
    scene = load_scene(args.scene)
    jobs = [j for j in scene.renderers if j.object.kind == 'mesh_renderer']
    job = next((j for j in jobs if args.object in (None, j.object.name)), None)
    if job is None:
        parser.error(f'--object must name a mesh renderer: {", ".join(j.object.name for j in jobs)}')
    key = made_from(args.scene, job, args.tolerance)
    key_path = args.out.with_name(args.out.name + '.key')
    if args.out.exists() and key_path.exists() and key_path.read_text().strip() == key:
        print(f'object_boxes: {args.out} is current', file=sys.stderr)
        return 0
    src = load_source(job.settings)
    objects, _ = masked_objects(src, job.settings)
    shapes = match_instances(objects, args.tolerance)
    lines = box_lines(shapes, job)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text('\n'.join(lines) + '\n')
    key_path.write_text(key + '\n')
    print(f'object_boxes: {len(lines)} objects, {len(shapes)} shapes -> {args.out}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
