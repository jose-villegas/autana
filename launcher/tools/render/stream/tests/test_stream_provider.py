"""Replay seeks, live feeds and record compatibility using the engine scene viewer.

    python -m unittest discover -s launcher/tools/render/stream/tests
"""
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

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools/device"))
import framerec  # noqa: E402
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
import showcase  # noqa: E402
from build.c_header import read_define  # noqa: E402

FRAMES = 61
SHORT_FRAMES = 10
SHORT_KEYFRAME_EVERY = 3
GENERATED_FRAMES = 2
FUTURE_RECORD_TYPE = 0xFFFF
BUDGET_TRIANGLES = 1200
SHOWCASE = showcase.showcase(showcase.load_scene(ROOT / "launcher/demo/sponza/sponza.scene.toml"))
RECORDED = {
    "herd": ["--scene", "capybara/herd"],
    "sponza": ["--scene", "sponza", "--object", SHOWCASE],
    "budget": ["--scene", "sponza", "--object", SHOWCASE, "--set", f"render.lod_triangle_budget={BUDGET_TRIANGLES}"],
}

FORMAT_HEADER = ROOT / "launcher/main/render/record/frame_record_format.h"
NO_PACK = read_define(FORMAT_HEADER, "FRAME_RECORD_NO_PACK")
NO_ENTRY = read_define(FORMAT_HEADER, "FRAME_RECORD_NO_ENTRY")
MATRIX_ELEMENTS = read_define(FORMAT_HEADER, "FRAME_RECORD_MATRIX")
GFX_HEADER = ROOT / "launcher/main/gfx/gfx.h"
PANEL = tuple(read_define(GFX_HEADER, name) for name in ("GFX_WIDTH", "GFX_HEIGHT"))
VIEW_BASIS = struct.Struct("<" + "3f" * 4 + "5f")
VIEWPORT = struct.Struct("<iii")


def frame_bytes(frame, rows=None, index=None, flags=None):
    payload = b"".join(framerec.HEAD.pack(framerec.RECORD_TYPES[name], len(body)) + body
                       for name, body in (frame.records if rows is None else rows))
    body = framerec.FRAME.pack(frame.index if index is None else index, frame.dt_ms,
                              frame.flags if flags is None else flags, frame.instances, frame.batches, len(payload))
    return framerec.HEAD.pack(framerec.RECORD_TYPES["FRAME"], len(body)) + body + payload


class StreamProvider(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix="stream_provider_"))
        script = ROOT / "launcher/tools/render/scenes/scene_viewer_render_host.sh"
        build = subprocess.run([shutil.which("sh"), str(script), "--build-only", "-o", str(cls.out)],
                               capture_output=True, text=True, timeout=900)
        if build.returncode:
            raise RuntimeError((build.stdout + build.stderr)[-4000:])
        cls.binary = pathlib.Path(re.search(r"^built (.+)$", build.stdout, re.MULTILINE)[1].strip())
        pack = subprocess.run([sys.executable, str(ROOT / "launcher/tools/r3d/build_pack.py"),
                               "-o", str(cls.out / "second-assets"), str(ROOT / "launcher/demo/capybara/herd.scene.toml")],
                              capture_output=True, text=True, timeout=600)
        if pack.returncode:
            raise RuntimeError((pack.stdout + pack.stderr)[-4000:])
        for path in (cls.out / "second-assets").glob("*.apak"):
            shutil.copy(path, cls.out / "assets" / path.name)
        cls.streams = {}
        for name, args in RECORDED.items():
            path = cls.out / (name + ".frec")
            run = subprocess.run([str(cls.binary), "--frames", str(FRAMES), "--record", str(path),
                                  "-o", str(cls.out / (name + ".bmp")), *args],
                                 capture_output=True, text=True, timeout=600, cwd=cls.out)
            if run.returncode:
                raise RuntimeError(run.stderr[-4000:])
            cls.streams[name] = framerec.parse(path.read_bytes())

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def stream(self, name):
        return self.streams[name]

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

    def test_bytes_that_are_no_stream_are_refused_at_once(self):
        noise = self.out / "noise.frec"
        noise.write_bytes(bytes([0xA5]) * 1000)
        run = subprocess.run([str(self.build_probe()), str(noise), "1000", str(self.out / "noise.bmp")],
                             capture_output=True, text=True, timeout=600,
                             env=dict(os.environ, AUTANA_ASSET_DIR=str(self.out / "assets")))
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr[-2000:])
        self.assertIn("HEADER must be first", run.stdout)

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
        run = subprocess.run([str(self.binary), *RECORDED["sponza"], "--frames", str(GENERATED_FRAMES),
                              "--framegen", "1", "--record", str(path), "-o", str(picture)],
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
            self.assertEqual(recorded_hash, replay_hash)

    def write_frames(self, name, *frames):
        path = self.out / (name + ".frec")
        header_end = framerec.HEAD.size + framerec.HEADER.size
        path.write_bytes((self.out / "herd.frec").read_bytes()[:header_end] + b"".join(frames))
        return path

    def test_invalid_dynamic_indices_are_refused(self):
        frame = self.stream("herd").frames[0]
        no_place = (NO_PACK, NO_ENTRY, 0)
        mesh = framerec.MESH.pack(0, framerec.MESH_KINDS["BUILT"], 0, 0, *no_place, *no_place, *no_place,
                                 *([0.0] * MATRIX_ELEMENTS))
        source = framerec.DYNMESH.pack(0, 1, 0, 1, 1, 1, 0)
        source += struct.pack("<3f3B3H", 0, 0, 1, 255, 255, 255, 65535, 65535, 65535)
        rows = [(name, body) for name, body in frame.records if name not in ("MESH", "POSE", "LODSTATE")]
        rows += [("MESH", mesh), ("DYNMESH", source)]
        run = self.replay_probe(self.write_frames("bad-index", frame_bytes(frame, rows)))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("dynamic triangle index", run.stderr)

    def test_oversized_view_and_choice_are_refused(self):
        frame = self.stream("herd").frames[0]
        for kind in ("VIEW", "CHOICE"):
            for axis in range(len(PANEL)):
                with self.subTest(kind=kind, axis=axis):
                    rows = []
                    for name, body in frame.records:
                        if name == kind:
                            if name == "VIEW":
                                body = bytearray(body)
                                viewport = list(VIEWPORT.unpack_from(body, VIEW_BASIS.size))
                                viewport[axis] = PANEL[axis] * 2
                                VIEWPORT.pack_into(body, VIEW_BASIS.size, *viewport)
                            else:
                                choice = list(framerec.CHOICE.unpack(body))
                                choice[-2 + axis] = PANEL[axis] * 2
                                body = framerec.CHOICE.pack(*choice)
                        rows.append((name, body))
                    run = self.replay_probe(self.write_frames("bad-size", frame_bytes(frame, rows)))
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn(kind + " size", run.stderr)

    def test_presentation_recordings_check_same(self):
        for present in ("scanline", "double"):
            with self.subTest(present=present):
                path = self.out / (present + ".frec")
                run = subprocess.run([str(self.binary), *RECORDED["sponza"], "--frames", "1",
                                      "--present", present, "--record", str(path),
                                      "-o", str(self.out / "present.bmp")],
                                     capture_output=True, text=True, timeout=600, cwd=self.out)
                self.assertEqual(run.returncode, 0, run.stderr[-4000:])
                run = self.replay_probe(path)
                self.assertEqual(run.returncode, 0, run.stderr[-4000:])
                self.assertIn("CHECK 0 SAME", run.stderr)

    def test_keyframe_resets_tunables_during_sequential_play(self):
        frame = self.stream("herd").frames[0]
        tune = ("TUNE", framerec.TUNE.pack(b"render.cull", 0))
        path = self.write_frames("tune-reset", frame_bytes(frame, [*frame.records, tune]),
                                 frame_bytes(frame, index=1))
        run = self.replay_probe(path)
        self.assertEqual(run.returncode, 0, run.stderr[-4000:])
        self.assertIn("TUNE render.cull -1", run.stdout)

    def test_generated_frame_without_kept_draw_is_skipped(self):
        frame = self.stream("herd").frames[0]
        generated = frame_bytes(frame, [(n, b) for n, b in frame.records if n in ("VIEW", "CHOICE")],
                                index=1, flags=framerec.GENERATED)
        path = self.write_frames("unkept", frame_bytes(frame), generated,
                                 frame_bytes(frame, [], index=2, flags=framerec.GENERATED))
        run = self.replay_probe(path)
        self.assertEqual(run.returncode, 0, run.stderr[-4000:])

    def test_generated_frame_before_first_keyframe_is_skipped(self):
        frame = self.stream("herd").frames[0]
        rows = [(name, body) for name, body in frame.records if name in ("VIEW", "CHOICE")]
        path = self.write_frames("initial-generated", frame_bytes(frame, rows, flags=framerec.GENERATED),
                                 frame_bytes(frame, index=1))
        run = self.replay_probe(path)
        self.assertEqual(run.returncode, 0, run.stderr[-4000:])

    def test_invalid_frame_state_is_refused(self):
        frame = self.stream("herd").frames[0]
        for missing in ("VIEW", "CHOICE"):
            with self.subTest(missing=missing):
                rows = [(name, body) for name, body in frame.records if name != missing]
                run = self.replay_probe(self.write_frames("missing-size", frame_bytes(frame, rows)))
                self.assertNotEqual(run.returncode, 0)
                self.assertIn("missing VIEW or CHOICE", run.stderr)
        empty = framerec.Frame(frame.index, frame.dt_ms, frame.flags, 0, 0, 0)
        rows = [(name, body) for name, body in frame.records if name in ("VIEW", "CHOICE")]
        run = self.replay_probe(self.write_frames("empty-draw", frame_bytes(empty, rows)))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("empty drawn frame", run.stderr)

    def test_more_distinct_packs_than_mounts(self):
        frame = self.stream("herd").frames[0]
        pack = next(body for name, body in frame.records if name == "PACK")
        slot, crc, original = framerec.PACK.unpack(pack)
        mounts = read_define(ROOT / "launcher/main/asset/asset_store.h", "ASSET_STORE_MOUNTS_MAX")
        frames = []
        for index in range(mounts + 1):
            name = f"turn-{index}"
            shutil.copy(self.out / "assets" / (framerec.text(original) + ".apak"),
                        self.out / "assets" / (name + ".apak"))
            rows = [(n, framerec.PACK.pack(slot, crc, name.encode()) if n == "PACK" else b)
                    for n, b in frame.records]
            check = self.stream("herd").checks[frame.index]
            rows.append(("CHECK", framerec.CHECK.pack(index, check["width"], check["height"], check["crc32"])))
            frames.append(frame_bytes(frame, rows, index=index))
        run = self.replay_probe(self.write_frames("many-packs", *frames))
        self.assertEqual(run.returncode, 0, run.stderr[-4000:])
        self.assertIn(f"RESULT {len(frames)} 0 ", run.stdout)

    def test_short_keyframe_interval(self):
        for name in ("herd", "sponza", "budget"):
            path = self.out / (name + "-short.frec")
            run = subprocess.run([str(self.binary), *RECORDED[name], "--frames", str(SHORT_FRAMES),
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

if __name__ == "__main__":
    unittest.main()
