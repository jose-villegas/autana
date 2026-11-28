"""Camera path edits leave geometry bake keys stable."""

import copy
import pathlib
import tempfile
import tomllib
import unittest
from collections import defaultdict

from test_bake import AnyStart, START_SHA256, write_fitted_tree
from anim_probe import build_sampler_once, write_camera_clip
from bake import bake
from r3d import build_pack
from r3d.import_settings import flight_camera, load_scene, load_visibility
from r3d.reference_set import reference_inputs
from test_r3d_import import PATH, REGION


EDITED_REACH = 37.0


def setUpModule():
    build_sampler_once()


def edit_camera_paths(scene, directory):
    edited = copy.deepcopy(scene)
    for camera in edited.objects:
        if camera.kind == "camera" and camera.component.path is not None:
            path = camera.component.path
            path.animation = write_camera_clip(directory, name=camera.name, reach=EDITED_REACH, node=path.node)
    return edited


def stage_digests(keys):
    return {kind: key for kind, key in keys.items()
            if kind != "reason" and not kind.endswith("_sha256")}


def changed_recipe_fields(before, after, prefix=""):
    for name in sorted(before.keys() | after.keys()):
        left, right = before.get(name), after.get(name)
        field = f"{prefix}{name}"
        if left == right:
            continue
        if isinstance(left, dict) and isinstance(right, dict):
            yield from changed_recipe_fields(left, right, f"{field}.")
        else:
            yield field


def geometry_key_changes(job, scene, edited, tools, starts, held, packed):
    sampled = bake.Poses({}, resample=True)
    before = bake.stage_keys(job, scene, tools, starts, held)
    after = bake.stage_keys(job, edited, tools, starts, sampled)
    problems = [keys["reason"] for keys in (before, after) if "reason" in keys]
    if problems:
        return problems

    before, after = stage_digests(before), stage_digests(after)
    changed = {kind: key for kind, key in before.items() if key is not None and key != after.get(kind)}
    if not changed:
        return []

    # A missing lock row must not hide a byte dependency.
    probe_starts = {key: starts.get(key, START_SHA256) for key in before.values() if key is not None}
    probe_before = stage_digests(bake.stage_keys(job, scene, tools, probe_starts, held))
    for kind, key in changed.items():
        if kind not in packed:
            probe = {**probe_starts, key: bake.digest([probe_starts[key]])}
            probe_after = stage_digests(bake.stage_keys(job, scene, tools, probe, held))
            if not any(other != kind and value != probe_after.get(other) for other, value in probe_before.items()):
                continue
        if kind in ("mesh", "start"):
            recipes = (bake.mesh_recipe(job, scene, tools, held), bake.mesh_recipe(job, edited, tools, sampled))
        elif kind in ("reference", "reference_pose"):
            recipes = tuple(bake.reference_recipe(reference_inputs(job, view), tools, poses)
                            for view, poses in ((scene, held), (edited, sampled)))
        else:
            detail = "key changed" if after.get(kind) is not None else "key became unavailable"
            problems.append(f"{kind} {detail} after its stage inputs changed")
            continue
        fields = ", ".join(changed_recipe_fields(*recipes))
        problems.append(f"{kind} key changed; changed inputs: {fields}")
    return problems


class CameraPathKeyTests(unittest.TestCase):
    def test_scene_renderers_ignore_camera_path_edits(self):
        packs, jobs = build_pack.pack_jobs([build_pack.DEFAULT_SEARCH])
        tables = bake.read_lock_tables()
        starts = bake.locked_starts(tables["bake"])
        held = bake.Poses(tables["poses"])
        tools = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
        packed = defaultdict(set)
        for item in bake.bakes_in(packs, jobs, starts, held):
            if item.packed:
                packed[item.tree.resolve()].add(item.kind)

        scenes = {source for entries in packs.values() for source in entries.values()
                  if source.name.endswith(build_pack.SCENE)}
        self.assertTrue(scenes, "pack search found no scenes")
        with tempfile.TemporaryDirectory() as directory:
            for index, path in enumerate(sorted(scenes)):
                scene = load_scene(path)
                clips = pathlib.Path(directory) / str(index)
                clips.mkdir()
                edited = edit_camera_paths(scene, clips)
                for job in scene.renderers:
                    with self.subTest(scene=bake.relative(scene.path), renderer=job.object.name):
                        changes = geometry_key_changes(
                            job, scene, edited, tools, starts, held, packed[job.asset_path.resolve()])
                        if changes:
                            self.fail("\n".join(changes))

    def test_checker_reports_path_visibility_and_accepts_region_probes(self):
        tools = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            scene = load_scene(write_fitted_tree(root, visibility=REGION, probe_fit=True))
            job = scene.renderers[0]
            clips = root / "edited"
            clips.mkdir()
            for visibility in (REGION, PATH):
                job.renderer.visibility = load_visibility(tomllib.loads(visibility)["visibility"])
                if visibility == PATH:
                    scene.camera.component.path = copy.deepcopy(flight_camera(scene).path)
                held = bake.Poses({})
                keys = stage_digests(bake.stage_keys(job, scene, tools, AnyStart(), held))
                self.assertTrue(all(keys.values()), keys)
                starts = dict.fromkeys(keys.values(), START_SHA256)
                edited = edit_camera_paths(scene, clips)
                changes = geometry_key_changes(job, scene, edited, tools, starts, held, {"levels"})
                if visibility == REGION:
                    self.assertEqual(changes, [])
                else:
                    self.assertIn("start key changed; changed inputs: camera.poses", changes)
