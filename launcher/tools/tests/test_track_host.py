"""anim/track_host.py: one cached build of track_host.c, rebuilt only when
what it is built from changes, safe to start from several runs at once on a
cold cache, and the poses it prints for a clip baked into a scratch pack.
Skipped where there is no C compiler or no sh."""

import pathlib
import shutil
import sys
import tempfile
import threading
import unittest
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from anim import track_host  # noqa: E402
from gltf import gltf_write  # noqa: E402


def write_clip(directory):
    """walk.anim.toml: node `camera` moving from x 0 to 2 over a second,
    facing glTF's -Z throughout."""
    directory = pathlib.Path(directory)
    channels = [{"node": 0, "path": "translation", "pointer": None, "interpolation": "LINEAR",
                 "times": [0.0, 1.0], "values": [(0.0, 0.0, 0.0), (2.0, 0.0, 0.0)]},
                {"node": 0, "path": "rotation", "pointer": None, "interpolation": "LINEAR",
                 "times": [0.0], "values": [(0.0, 0.0, 0.0, 1.0)]}]
    (directory / "walk.glb").write_bytes(gltf_write.build_glb([{"name": "camera"}], [{"name": "walk", "channels": channels}]))
    clip = directory / "walk.anim.toml"
    clip.write_text('source = "walk.glb"\nanimation = "walk"\n')
    return clip


def has_compiler():
    try:
        track_host.compiler()
    except track_host.TrackHostError:
        return False
    return True


@unittest.skipUnless(shutil.which("sh") and has_compiler(), "needs sh and a C compiler")
class TrackHostTests(unittest.TestCase):
    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp())
        self.build = self.dir / "build"
        self.patch = mock.patch.object(track_host, "BUILD", self.build)
        self.patch.start()
        self.clip = write_clip(self.dir)

    def tearDown(self):
        self.patch.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_poses_are_the_clip_sampled_every_step_at_the_lens_given(self):
        text = track_host.poses(self.clip, "camera", 250, 184, 224, 0.62, 6.0)
        lines = text.splitlines()
        self.assertEqual(lines[:2], ["size 184 224", "lens 0.62 6.0"])
        poses = [[float(value) for value in line.split()[1:]] for line in lines[2:]]
        self.assertTrue(all(line.startswith("pose ") for line in lines[2:]))
        self.assertEqual(len(poses), 4)
        for pose, x in zip(poses, (0.0, 0.5, 1.0, 1.5)):
            for got, want in zip(pose, (x, 0, 0, 0, 0, -1)):
                self.assertAlmostEqual(got, want, places=6)

    def test_runs_started_together_on_a_cold_cache_all_succeed_and_leave_one_program(self):
        results, errors = [], []

        def poses():
            try:
                results.append(track_host.poses(self.clip, "camera", 100, 8, 6, 0.5, 1.0))
            except Exception as error:  # noqa: BLE001 - the test reports whatever a run raised
                errors.append(error)

        runs = [threading.Thread(target=poses) for _ in range(4)]
        for run in runs:
            run.start()
        for run in runs:
            run.join()
        self.assertEqual(errors, [])
        self.assertEqual(len(results), 4)
        self.assertEqual(len(set(results)), 1)
        self.assertEqual([path.name for path in self.build.iterdir()], [track_host.program().name])

    def test_a_warm_cache_runs_the_same_program_without_building(self):
        built = track_host.program()
        stamp = built.stat().st_mtime_ns
        with mock.patch.object(track_host.subprocess, "run", wraps=track_host.subprocess.run) as run:
            self.assertEqual(track_host.program(), built)
        compiles = [call for call in run.call_args_list if "-o" in call.args[0]]
        self.assertEqual(compiles, [])
        self.assertEqual(built.stat().st_mtime_ns, stamp)

    def test_a_changed_source_is_another_program(self):
        cc = track_host.compiler()
        before = track_host.build_key(cc)
        copy = self.dir / "track_host.c"
        copy.write_bytes(track_host.SOURCES[0].read_bytes() + b"\n")
        with mock.patch.object(track_host, "SOURCES", (copy, *track_host.SOURCES[1:])):
            self.assertNotEqual(track_host.build_key(cc), before)
        self.assertNotEqual(track_host.build_key(cc + " "), before, "the compiler is part of the key")

    def test_a_pack_and_an_id_pass_through_as_track_host_takes_them(self):
        pack = self.dir / "walk.apak"
        pack.write_bytes(track_host.clip_pack(self.clip))
        self.assertEqual(track_host.run(["--pack", pack, "--clip", "walk", "--every", 500]),
                         track_host.sample(self.clip, ["--every", 500]))

    def test_a_missing_clip_or_track_is_an_error_naming_it(self):
        pack = self.dir / "walk.apak"
        pack.write_bytes(track_host.clip_pack(self.clip))
        with self.assertRaisesRegex(track_host.TrackHostError, "clip run"):
            track_host.run(["--pack", pack, "--clip", "run"])
        with self.assertRaisesRegex(track_host.TrackHostError, "no track lens/translation"):
            track_host.poses(self.clip, "lens", 100, 8, 6, 0.5, 1.0)

    def test_the_command_line_takes_an_anim_toml(self):
        with mock.patch.object(sys, "stdout") as out:
            self.assertEqual(track_host.main([str(self.clip), "--every", "500"]), 0)
        printed = "".join(call.args[0] for call in out.write.call_args_list)
        self.assertEqual(printed, "0 camera/translation 0 0 0\n0 camera/rotation 0 0 0 1\n"
                                  "500 camera/translation 1 0 0\n500 camera/rotation 0 0 0 1\n")


if __name__ == "__main__":
    unittest.main()
