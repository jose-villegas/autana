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
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import struct
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
SHORT_FRAMES = 10
SHORT_KEYFRAME_EVERY = 3
GENERATED_FRAMES = 2
FUTURE_RECORD_TYPE = 0xFFFF
BUDGET_TRIANGLES = 1200
RECORDED = {
    "herd": ["--scene", "herd-8"],
    "water": ["--quarter", "0", "--scene", "water", "--console", "0:water tour"],
    "sponza": ["--scene", "sponza-fitted-full"],
    "budget": ["--scene", "sponza-fitted-full", "--set", f"render.lod_triangle_budget={BUDGET_TRIANGLES}"],
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
        cls.out = pathlib.Path(os.environ.get("AUTANA_RECORD_TEST_OUT") or tempfile.mkdtemp(prefix="record_host_"))
        cls.out.mkdir(parents=True, exist_ok=True)
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
        if not os.environ.get("AUTANA_RECORD_TEST_OUT"):
            shutil.rmtree(cls.out, ignore_errors=True)

    def stream(self, name):
        self.assertIsNone(self.failure, self.failure)
        self.assertTrue(name in self.streams, self.logs.get(name, "")[-2000:])
        return self.streams[name]

    def test_stream_replay_matches_every_check(self):
        self.assertIsNone(self.failure, self.failure)
        shell = shutil.which("sh") or shutil.which("bash")
        script = ROOT / "launcher/tools/render/stream_render.sh"
        built = subprocess.run([shell, str(script), "--build-only", "-o", str(self.out / "replay")],
                               capture_output=True, text=True, timeout=900)
        self.assertEqual(built.returncode, 0, (built.stdout + built.stderr)[-2000:])
        binary = re.search(r"^built (.+)$", built.stdout, re.MULTILINE)
        self.assertIsNotNone(binary, built.stdout)
        for name in RECORDED:
            with self.subTest(scene=name):
                stream = self.stream(name)
                env = dict(os.environ, AUTANA_ASSET_DIR=str(self.out / "assets"))
                run = subprocess.run([binary[1].strip(), "--stream", str(self.out / (name + ".frec")),
                                      "--check", "--quarter", "0" if name == "water" else "1",
                                      "-o", str(self.out / (name + "-replay.bmp"))],
                                     capture_output=True, text=True, timeout=600, env=env)
                self.assertEqual(run.returncode, 0, run.stderr[-4000:])
                matched = re.findall(r"CHECK (\d+) SAME", run.stderr)
                self.assertEqual([int(i) for i in matched], sorted(stream.checks), run.stderr[-4000:])

        data = bytearray((self.out / "herd.frec").read_bytes())
        at = 0
        while at < len(data):
            kind, length = framerec.HEAD.unpack_from(data, at)
            body = at + framerec.HEAD.size
            if kind == framerec.RECORD_TYPES["CHECK"]:
                index, width, height, crc = framerec.CHECK.unpack_from(data, body)
                data[body:body + framerec.CHECK.size] = framerec.CHECK.pack(index, width, height, crc ^ 1)
                break
            at = body + length
        changed = self.out / "wrong-check.frec"
        changed.write_bytes(data)
        run = subprocess.run([binary[1].strip(), "--stream", str(changed), "--check", "--frames", "1",
                              "-o", str(self.out / "wrong-check.bmp")],
                             capture_output=True, text=True, env=env, timeout=600)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("CHECK 0 DIFFERS", run.stderr)

    @classmethod
    def build_probe(cls):
        if hasattr(cls, "probe"):
            return cls.probe
        project = ROOT / "launcher/tools/render/stream/tests"
        build_dir = cls.out / "feed-build"
        for command in (["cmake", "-S", str(project), "-B", str(build_dir)],
                        ["cmake", "--build", str(build_dir), "--target", "stream_feed_probe", "--parallel"]):
            run = subprocess.run(command, capture_output=True, text=True, timeout=900)
            assert run.returncode == 0, (run.stdout + run.stderr)[-2000:]
        cls.probe = build_dir / ("stream_feed_probe.exe" if os.name == "nt" else "stream_feed_probe")
        return cls.probe

    def test_piecewise_feed_and_pack_identity(self):
        self.assertIsNone(self.failure, self.failure)
        probe = self.build_probe()
        env = dict(os.environ, AUTANA_ASSET_DIR=str(self.out / "assets"))
        path = self.out / "herd.frec"
        results = []
        for chunk in (1, 7, path.stat().st_size):
            run = subprocess.run([str(probe), str(path), str(chunk), str(self.out / "feed.bmp")],
                                 capture_output=True, text=True, env=env, timeout=600)
            self.assertEqual(run.returncode, 0, run.stderr[-4000:])
            results.append(run.stdout)
            self.assertIn(f"INDEX {FRAMES} 0 ", run.stdout)
            self.assertIn(f"RESULT {len(self.stream('herd').checks)} 0 ", run.stdout)
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0], results[2])
        data = bytearray(path.read_bytes())
        at = 0
        pack_name = None
        while at < len(data):
            kind, length = framerec.HEAD.unpack_from(data, at)
            body = at + framerec.HEAD.size
            if kind == framerec.RECORD_TYPES["PACK"]:
                slot, crc, name = framerec.PACK.unpack_from(data, body)
                pack_name = name.split(b"\0", 1)[0].decode()
                data[body:body + framerec.PACK.size] = framerec.PACK.pack(slot, crc ^ 1, name)
                break
            at = body + length
        self.assertIsNotNone(pack_name)
        bad = self.out / "wrong-crc.frec"
        bad.write_bytes(data)
        run = subprocess.run([str(probe), str(bad), "7", str(self.out / "bad.bmp")],
                             capture_output=True, text=True, env=env, timeout=600)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn(pack_name, run.stderr)
        self.assertIn("crc32 stream=", run.stderr)

    def replay_probe(self, path, target=None):
        args = [str(self.build_probe()), str(path), "7", str(self.out / "probe.bmp")]
        if target is not None:
            args.append(str(target))
        return subprocess.run(args, capture_output=True, text=True, timeout=600,
                              env=dict(os.environ, AUTANA_ASSET_DIR=str(self.out / "assets")))

    def test_seek_each_keyframe_from_fresh_decode(self):
        for name in ("herd", "sponza", "budget"):
            for frame in self.stream(name).frames:
                if not frame.keyframe:
                    continue
                with self.subTest(scene=name, keyframe=frame.index):
                    run = self.replay_probe(self.out / (name + ".frec"), frame.index)
                    self.assertEqual(run.returncode, 0, run.stderr[-4000:])
                    self.assertIn(f"CHECK {frame.index} SAME", run.stderr)

    def test_unknown_record_between_frames(self):
        data = (self.out / "herd.frec").read_bytes()
        header_end = framerec.HEAD.size + framerec.HEADER.size
        path = self.out / "unknown.frec"
        payload = b"new"
        path.write_bytes(data[:header_end] + framerec.HEAD.pack(FUTURE_RECORD_TYPE, len(payload))
                         + payload + data[header_end:])
        run = self.replay_probe(path)
        self.assertEqual(run.returncode, 0, run.stderr[-4000:])

    def test_pack_slot_renamed_between_frames(self):
        first = (self.out / "herd.frec").read_bytes()
        second = bytearray((self.out / "sponza.frec").read_bytes())
        at = framerec.HEAD.size + framerec.HEADER.size
        while at < len(second):
            kind, length = framerec.HEAD.unpack_from(second, at)
            body = at + framerec.HEAD.size
            if kind in (framerec.RECORD_TYPES["FRAME"], framerec.RECORD_TYPES["CHECK"]):
                index, = struct.unpack_from("<I", second, body)
                struct.pack_into("<I", second, body, index + FRAMES)
            at = body + length
        path = self.out / "renamed.frec"
        path.write_bytes(first + second[framerec.HEAD.size + framerec.HEADER.size:])
        run = self.replay_probe(path)
        self.assertEqual(run.returncode, 0, run.stderr[-4000:])
        self.assertIn(f"RESULT {len(self.stream('herd').checks) + len(self.stream('sponza').checks)} 0 ", run.stdout)

    def test_generated_picture_and_seek(self):
        path = self.out / "generated.frec"
        picture = self.out / "generated.bmp"
        run = subprocess.run([str(self.binary), "--no-hud", "--scene", "stress-open", "--frames", str(GENERATED_FRAMES),
                              "--set", "render_lab.framegen=1", "--record", str(path), "-o", str(picture)],
                             capture_output=True, text=True, timeout=600, cwd=self.out)
        self.assertEqual(run.returncode, 0, run.stderr[-4000:])
        stream = framerec.parse(path.read_bytes())
        self.assertTrue(stream.frames[-1].flags & framerec.GENERATED)
        for target in (None, GENERATED_FRAMES - 1):
            run = self.replay_probe(path, target)
            self.assertEqual(run.returncode, 0, run.stderr[-4000:])
            def picture_hash(path):
                bmp = path.read_bytes()
                offset, = struct.unpack_from("<I", bmp, 10)
                return hashlib.sha256(bmp[offset:]).hexdigest()
            recorded_hash = picture_hash(picture)
            replay_hash = picture_hash(self.out / "probe.bmp")
            print(f"GENERATED seek={target} recorded={recorded_hash} replay={replay_hash}")
            self.assertEqual(recorded_hash, replay_hash)

    def test_short_keyframe_interval(self):
        for name in ("herd", "sponza", "budget"):
            path = self.out / (name + "-short.frec")
            run = subprocess.run([str(self.binary), "--no-hud", *RECORDED[name], "--frames", str(SHORT_FRAMES),
                                  "--record-keyframe-every", str(SHORT_KEYFRAME_EVERY), "--record", str(path),
                                  "-o", str(self.out / "short.bmp")],
                                 capture_output=True, text=True, timeout=600, cwd=self.out)
            self.assertEqual(run.returncode, 0, run.stderr[-4000:])
            stream = framerec.parse(path.read_bytes())
            self.assertEqual([f.index for f in stream.frames if f.keyframe], list(range(0, SHORT_FRAMES, SHORT_KEYFRAME_EVERY)))
            for frame in stream.frames:
                if frame.keyframe:
                    run = self.replay_probe(path, frame.index)
                    self.assertEqual(run.returncode, 0, run.stderr[-4000:])
                    self.assertIn(f"CHECK {frame.index} SAME", run.stderr)

    def test_keyframe_carries_pre_draw_lod_state(self):
        for name in ("herd", "sponza", "budget"):
            for frame in self.stream(name).frames:
                if frame.keyframe:
                    records = [body for kind, body in frame.records if kind == "LODSTATE"]
                    self.assertEqual(len(records), 1, (name, frame.index))
                    phase, tolerance = struct.unpack_from("<If", records[0])
                    self.assertEqual(phase, frame.index, (name, frame.index))
                    if name == "budget" and frame.index > 0:
                        self.assertGreater(tolerance, 0)
                    print(f"LODSTATE scene={name} keyframe={frame.index} bytes={framerec.HEAD.size + len(records[0])}")

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
