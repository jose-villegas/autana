"""anim/track_host.py: one cached build of track_host.c, rebuilt only when
what it is built from changes, safe to start from several runs at once on a
cold cache, and the poses it prints for a clip baked into a scratch pack.
Skipped where there is no C compiler or no sh."""

import contextlib
import io
import math
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))

from anim import track_host  # noqa: E402
from anim_probe import has_compiler, write_camera_clip  # noqa: E402


def read_poses(text):
    lines = text.splitlines()
    return lines[:2], [[float(value) for value in line.split()[1:]] for line in lines[2:] if line.startswith("pose ")]


@unittest.skipUnless(has_compiler(), "needs sh and a C compiler")
class TrackHostTests(unittest.TestCase):
    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp())
        self.build = self.dir / "build"
        self.patch = mock.patch.object(track_host, "BUILD", self.build)
        self.patch.start()
        self.clip = write_camera_clip(self.dir, name="walk", reach=2.0)

    def tearDown(self):
        self.patch.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_poses_are_the_clip_sampled_every_step_at_the_lens_given(self):
        head, poses = read_poses(track_host.poses(self.clip, "camera", 250, 184, 224, 0.62, 6.0))
        self.assertEqual(head, ["size 184 224", "lens 0.62 6.0"])
        self.assertEqual(len(poses), 4)
        for pose, x in zip(poses, (0.0, 0.5, 1.0, 1.5)):
            for got, want in zip(pose, (x, 0, 0, 0, 0, -1)):
                self.assertAlmostEqual(got, want, places=6)

    def test_a_pose_looks_along_minus_z_turned_by_the_node_s_rotation_between_keys(self):
        clip = write_camera_clip(self.dir, name="turn", degrees=90.0)
        _, poses = read_poses(track_host.poses(clip, "camera", 250, 8, 6, 0.5, 1.0))
        self.assertEqual(len(poses), 4)
        for pose, degrees in zip(poses, (0.0, 22.5, 45.0, 67.5)):
            angle = math.radians(degrees)
            for got, want in zip(pose[3:], (-math.sin(angle), 0.0, -math.cos(angle))):
                self.assertAlmostEqual(got, want, places=5, msg=f"forward at {degrees} degrees")

    def host(self, *args):
        """track_host over the walk clip's pack, bounded in time: a run that
        never ends fails the test instead of hanging the suite."""
        return subprocess.run([str(track_host.program()), "--pack", str(self.pack()), "--clip", "walk", *map(str, args)],
                              capture_output=True, text=True, timeout=60)

    def test_poses_refuse_a_start_or_a_clamp_they_would_ignore_and_no_mode_takes_a_zero_step(self):
        for args in (["--from", 250, "--poses", "camera", 8, 6, 0.5, 1.0], ["--clamp", "--poses", "camera", 8, 6, 0.5, 1.0],
                     ["--every", 0, "--poses", "camera", 8, 6, 0.5, 1.0], ["--every", 0]):
            done = self.host(*args)
            self.assertEqual(done.returncode, 2, args)
            self.assertIn("usage", done.stderr, args)

    def test_a_step_past_the_end_of_the_u32_clock_ends_the_run(self):
        top = 0xFFFFFFFF
        for args, lines in ((["--every", 1 << 30, "--until", top, "--poses", "camera", 8, 6, 0.5, 1.0], 2 + 4),
                            (["--from", top - 300, "--every", 200, "--until", top], 2 * 2)):
            done = self.host(*args)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(len(done.stdout.splitlines()), lines, args)
        times = sorted({int(line.split()[0]) for line in done.stdout.splitlines()})
        self.assertEqual(times, [top - 300, top - 100])

    def test_poses_past_the_clip_s_end_loop_back_to_its_start(self):
        _, poses = read_poses(self.host("--every", 500, "--until", 2000, "--poses", "camera", 8, 6, 0.5, 1.0).stdout)
        self.assertEqual([round(pose[0], 6) for pose in poses], [0.0, 1.0, 0.0, 1.0])

    def test_a_header_listing_the_compiler_refuses_is_an_error(self):
        with mock.patch.object(track_host, "FLAGS", (*track_host.FLAGS, "-include", str(self.dir / "missing.h"))), \
                self.assertRaisesRegex(track_host.TrackHostError, "listing track_host's headers"):
            track_host.inputs(track_host.compiler())

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

    def test_a_build_for_another_key_leaves_the_program_a_run_already_holds(self):
        held = track_host.program()
        with mock.patch.object(track_host, "build_key", return_value="other"):
            other = track_host.program()
        self.assertNotEqual(other, held)
        self.assertTrue(held.is_file(), "a run that found this program must still be able to start it")
        self.assertEqual(track_host.run(["--pack", self.pack(), "--clip", "walk"]), track_host.sample(self.clip, []))

    def test_a_warm_cache_runs_the_same_program_without_building(self):
        built = track_host.program()
        stamp = built.stat().st_mtime_ns
        with mock.patch.object(track_host.subprocess, "run", wraps=track_host.subprocess.run) as run:
            self.assertEqual(track_host.program(), built)
        compiles = [call for call in run.call_args_list if "-o" in call.args[0]]
        self.assertEqual(compiles, [])
        self.assertEqual(built.stat().st_mtime_ns, stamp)

    def test_the_key_covers_every_header_the_compiler_reads_the_compiler_and_the_flags(self):
        cc = track_host.compiler()
        inputs = track_host.inputs(cc)
        names = {path.name for path in inputs}
        self.assertLessEqual({path.name for path in track_host.SOURCES}, names)
        self.assertLessEqual({"anim_tracks.h", "asset_bytes.h", "quatf.h", "vec3f.h"}, names)
        before = track_host.build_key(cc)
        header = next(path for path in inputs if path.name == "quatf.h")
        edited = self.dir / header.name
        edited.write_bytes(header.read_bytes() + b"\n")
        with mock.patch.object(track_host, "inputs", return_value=[edited if p == header else p for p in inputs]):
            self.assertNotEqual(track_host.build_key(cc), before, "a header is part of the key")
        with mock.patch.object(track_host, "FLAGS", (*track_host.FLAGS, "-DX")):
            self.assertNotEqual(track_host.build_key(cc), before, "the flags are part of the key")
        with mock.patch.object(track_host, "inputs", return_value=inputs):
            self.assertNotEqual(track_host.build_key(cc + " "), before, "the compiler is part of the key")

    def test_a_failed_build_raises_with_the_compiler_s_words_and_leaves_nothing(self):
        broken = self.dir / "track_host.c"
        broken.write_bytes(track_host.SOURCES[0].read_bytes() + b"\nint broken(void) { return }\n")
        with mock.patch.object(track_host, "SOURCES", (broken, *track_host.SOURCES[1:])), \
                self.assertRaisesRegex(track_host.TrackHostError, "building track_host"):
            track_host.program()
        self.assertEqual(list(self.build.iterdir()), [])

    def test_the_build_compiles_with_the_flags(self):
        # Preprocesses cleanly, so the header listing passes; only a build that applies it fails to link.
        with mock.patch.object(track_host, "FLAGS", (*track_host.FLAGS, "-Dmain=track_host_no_main")), \
                self.assertRaisesRegex(track_host.TrackHostError, "building track_host"):
            track_host.program()

    def test_a_file_the_flags_pull_in_is_in_the_key(self):
        extra = self.dir / "extra.h"
        extra.write_text("#define TRACK_HOST_EXTRA 1\n")
        with mock.patch.object(track_host, "FLAGS", (*track_host.FLAGS, "-include", str(extra))):
            self.assertIn(extra.resolve(), track_host.inputs(track_host.compiler()))

    def pack(self):
        pack = self.dir / "walk.apak"
        pack.write_bytes(track_host.clip_pack(self.clip))
        return pack

    def test_a_pack_and_an_id_pass_through_as_track_host_takes_them(self):
        self.assertEqual(track_host.run(["--pack", self.pack(), "--clip", "walk", "--every", 500]),
                         track_host.sample(self.clip, ["--every", 500]))

    def test_a_missing_clip_or_track_is_an_error_naming_it(self):
        pack = self.pack()
        with self.assertRaisesRegex(track_host.TrackHostError, "clip run"):
            track_host.run(["--pack", pack, "--clip", "run"])
        with self.assertRaisesRegex(track_host.TrackHostError, "node lens: "):
            track_host.poses(self.clip, "lens", 100, 8, 6, 0.5, 1.0)

    def test_the_command_line_takes_an_anim_toml(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(track_host.main([str(self.clip), "--every", "500"]), 0)
        self.assertEqual(out.getvalue(), "0 camera:TRNS.position 0 0 0\n0 camera:TRNS.rotation 0 0 0 1\n"
                                         "500 camera:TRNS.position 1 0 0\n500 camera:TRNS.rotation 0 0 0 1\n")

    def test_the_command_line_fails_with_status_2_naming_the_tool(self):
        for argv in (["--pack", str(self.dir / "none.apak"), "--clip", "walk"], [str(self.dir / "none.anim.toml")]):
            err = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(track_host.main(argv), 2, argv)
            self.assertTrue(err.getvalue().startswith("track_host: "), err.getvalue())


if __name__ == "__main__":
    unittest.main()
