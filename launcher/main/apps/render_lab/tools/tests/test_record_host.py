"""A host render's --record, read back by launcher/tools/device/framerec.py.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests

Builds the render lab's host renderer once, records three scenes and parses
each stream: one FRAME per frame drawn, a keyframe every keyframe_every
frames carrying a MESH for every drawn mesh and naming its packs (a scene's
resident copies of pack meshes by the entries they copy), a CHECK
for each keyframe, POSE between keyframes while the herd animates, and a
DYNMESH every frame the water rebuilds its surface.
"""
import math
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[6]
sys.path.insert(0, str(ROOT / "launcher/tools/device"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import framerec  # noqa: E402
import host_render  # noqa: E402

# Two keyframe intervals and one frame: the third keyframe is the last frame.
FRAMES = 61
RECORDED = {
    "herd": ["--scene", "herd-8"],
    "water": ["--quarter", "0", "--scene", "water", "--console", "0:water tour"],
    "sponza": ["--scene", "sponza-fitted-full"],
}


def build(out):
    shell = shutil.which("sh") or shutil.which("bash")
    run = subprocess.run([shell, str(host_render.SCRIPT), "-o", str(out), "--build-only"], capture_output=True,
                         text=True, timeout=900)
    built = re.search(r"^built (.+)$", run.stdout, re.MULTILINE)
    return run, (pathlib.Path(built[1].strip()) if built else None)


class RecordHost(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix="record_host_"))
        run, cls.binary = build(cls.out)
        cls.failure = None if cls.binary else (run.stdout + run.stderr)[-2000:]
        cls.streams, cls.logs = {}, {}
        for name, args in RECORDED.items() if cls.binary else ():
            path = cls.out / (name + ".frec")
            run = subprocess.run([str(cls.binary), "--no-hud", "--frames", str(FRAMES), "--record", str(path), "-o",
                                  str(cls.out / (name + ".bmp")), *args], capture_output=True, text=True,
                                 timeout=600, cwd=cls.out)
            cls.logs[name] = run.stderr
            if run.returncode == 0:
                cls.streams[name] = framerec.parse(path.read_bytes())

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def stream(self, name):
        self.assertIsNone(self.failure, self.failure)
        self.assertTrue(name in self.streams, self.logs.get(name, "")[-2000:])
        return self.streams[name]

    def test_each_drawn_frame_is_recorded_and_none_dropped(self):
        for name in RECORDED:
            with self.subTest(scene=name):
                stream = self.stream(name)
                self.assertEqual([f.index for f in stream.frames], list(range(FRAMES)))
                self.assertIn(f"frames={FRAMES} dropped=0", self.logs[name])
                self.assertFalse(any(f.flags & framerec.AFTER_DROP for f in stream.frames))

    def test_every_keyframe_carries_a_mesh_for_each_drawn_mesh_and_names_its_packs(self):
        for name in RECORDED:
            with self.subTest(scene=name):
                stream = self.stream(name)
                every = stream.header["keyframe_every"]
                keyframes = [f for f in stream.frames if f.keyframe]
                self.assertEqual([f.index for f in keyframes], list(range(0, FRAMES, every)))
                self.assertEqual(len(keyframes), math.ceil(FRAMES / every))
                for f in keyframes:
                    meshes = [framerec.decode_mesh(body) for kind, body in f.records if kind == "MESH"]
                    self.assertEqual(sorted(m["index"] for m in meshes), list(range(f.instances + f.batches)))
                    named = {framerec.PACK.unpack(body)[0] for kind, body in f.records if kind == "PACK"}
                    used = {m["positions"][0] for m in meshes} - {0xFF}
                    self.assertEqual(named, used)

    def test_a_scene_s_resident_meshes_are_named_by_the_pack_entries_they_copy(self):
        meshes = [framerec.decode_mesh(body) for kind, body in self.stream("sponza").frames[0].records
                  if kind == "MESH"]
        self.assertTrue(meshes)
        for m in meshes:
            with self.subTest(mesh=m["index"]):
                self.assertNotEqual(m["positions"][0], 0xFF)
                self.assertNotEqual(m["colours"][0], 0xFF)
                if m["kind"] == "BATCH":
                    self.assertNotEqual(m["batch"][0], 0xFF)

    def test_each_keyframe_s_picture_is_hashed(self):
        for name in RECORDED:
            with self.subTest(scene=name):
                stream = self.stream(name)
                self.assertEqual(sorted(stream.checks), [f.index for f in stream.frames if f.keyframe])

    def test_the_herd_s_poses_and_the_water_s_surface_ride_between_keyframes(self):
        between = [f for f in self.stream("herd").frames if not f.keyframe]
        self.assertTrue(any(kind == "POSE" for f in between for kind, _ in f.records))
        water = self.stream("water").frames
        self.assertTrue(all(any(kind == "DYNMESH" for kind, _ in f.records) for f in water))


if __name__ == "__main__":
    unittest.main()
