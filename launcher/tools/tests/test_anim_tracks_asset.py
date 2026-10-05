"""The animation tracks entry (anim/tracks_asset.py): what it writes reads
back as baked, what it refuses to write or read, and build_pack finding every
.anim.toml. The firmware's reader of the same bytes is suite_anim_tracks.c."""

import io
import json
import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))

from anim import tracks_asset  # noqa: E402
from anim.tracks_asset import HEADER, ROW, TracksError  # noqa: E402
from anim_probe import channel, probe_entry, probe_glb  # noqa: E402
from asset.asset_pack import parse_pack  # noqa: E402
from gltf import gltf_read, gltf_write  # noqa: E402
from r3d import build_pack  # noqa: E402


def single(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def probe_tracks():
    document, binary = gltf_read.parse_glb(probe_glb())
    return tracks_asset.clip_tracks(document, binary, document["animations"][0])


def edited(entry, offset, fmt, value):
    out = bytearray(entry)
    struct.pack_into(fmt, out, offset, value)
    return bytes(out)


def row_at(index):
    return HEADER.size + ROW.size * index


class RoundTripTests(unittest.TestCase):
    def test_every_track_reads_back_as_baked_in_single_precision(self):
        tracks, duration_ms = probe_tracks()
        decoded, decoded_ms = tracks_asset.decode(tracks_asset.encode(tracks, duration_ms))
        self.assertEqual(decoded_ms, 3000)
        self.assertEqual([t["name"] for t in decoded], [t["name"] for t in tracks])
        for want, got in zip(tracks, decoded):
            self.assertEqual(got["interpolation"], want["interpolation"], want["name"])
            self.assertEqual(got["quaternion"], want["quaternion"], want["name"])
            self.assertEqual(got["times"], [single(t) for t in want["times"]], want["name"])
            self.assertEqual(got["values"], [tuple(single(x) for x in row) for row in want["values"]], want["name"])

    def test_the_same_clip_bakes_the_same_bytes(self):
        self.assertEqual(probe_entry(), probe_entry())

    def test_the_rows_name_4_aligned_arrays_after_the_table(self):
        entry = probe_entry()
        version, count, duration_ms = HEADER.unpack_from(entry)
        self.assertEqual((version, count, duration_ms), (tracks_asset.VERSION, 8, 3000))
        for index in range(count):
            _, times_at, values_at, _, _, _, _ = ROW.unpack_from(entry, row_at(index))
            for offset in (times_at, values_at):
                self.assertEqual(offset % 4, 0)
                self.assertGreaterEqual(offset, row_at(count))

    def test_a_channel_that_never_changes_is_one_key(self):
        tracks, _ = probe_tracks()
        held = next(t for t in tracks if t["name"] == "hand/translation")
        self.assertEqual(held["times"], [0.0])

    def test_a_name_holding_a_nul_is_refused(self):
        tracks, duration_ms = probe_tracks()
        tracks[0] = dict(tracks[0], name="a\0b/translation")
        with self.assertRaises(TracksError):
            tracks_asset.encode(tracks, duration_ms)

    def test_a_name_of_31_bytes_bakes_and_one_of_32_fails_naming_the_track(self):
        def baked(node):
            glb = gltf_write.build_glb([{"name": node}], [{"name": "clip", "channels": [
                channel(0, "translation", [0.0, 1.0], [(0, 0, 0), (1, 1, 1)])]}])
            document, binary = gltf_read.parse_glb(glb)
            return tracks_asset.encode(*tracks_asset.clip_tracks(document, binary, document["animations"][0]))

        fits = "n" * 19  # with "/translation", 31 bytes
        self.assertEqual(tracks_asset.decode(baked(fits))[0][0]["name"], fits + "/translation")
        with self.assertRaisesRegex(TracksError, "n" * 20 + "/translation"):
            baked("n" * 20)


class RefusalTests(unittest.TestCase):
    def assert_refused(self, entry):
        with self.assertRaises(TracksError):
            tracks_asset.decode(entry)

    def test_a_short_entry_or_a_truncated_table_is_refused(self):
        entry = probe_entry()
        self.assert_refused(entry[:4])
        self.assert_refused(entry[:row_at(7) + 10])

    def test_an_unknown_version_is_refused(self):
        self.assert_refused(edited(probe_entry(), 0, "<H", tracks_asset.VERSION + 1))

    def test_an_array_past_the_end_or_misaligned_or_in_the_table_is_refused(self):
        entry = probe_entry()
        self.assert_refused(edited(entry, row_at(0) + 36, "<I", len(entry) - 4))
        self.assert_refused(edited(entry, row_at(0) + 32, "<I", ROW.unpack_from(entry, row_at(0))[1] + 2))
        self.assert_refused(edited(entry, row_at(0) + 32, "<I", 4))

    def test_a_bad_width_interpolation_flag_or_pad_is_refused(self):
        entry = probe_entry()
        for offset, value in ((42, 0), (42, 5), (43, 3), (44, 2), (45, 1)):
            self.assert_refused(edited(entry, row_at(0) + offset, "<B", value))

    def test_a_quaternion_that_is_not_four_wide_is_refused(self):
        self.assert_refused(edited(probe_entry(), row_at(0) + 44, "<B", 1))  # lamp/translation is 3 wide

    def test_an_unterminated_name_is_refused(self):
        self.assert_refused(edited(probe_entry(), row_at(0), "<32s", b"x" * 32))


    def test_zero_keys_and_each_padding_byte_are_refused(self):
        entry = probe_entry()
        self.assert_refused(edited(entry, row_at(0) + 40, "<H", 0))
        for offset in (45, 46, 47):
            self.assert_refused(edited(entry, row_at(0) + offset, "<B", 1))


class CheckTests(unittest.TestCase):
    """What the bake refuses or reshapes before a track is written."""

    def assert_refused(self, held):
        with self.assertRaises(TracksError):
            tracks_asset.check("n/path", held)

    def test_a_key_that_is_not_finite_is_refused(self):
        for bad in (float("nan"), float("inf")):
            self.assert_refused(channel(0, "translation", [0.0, 1.0], [(0, 0, 0), (bad, 0, 0)]))
            self.assert_refused(channel(0, "translation", [0.0, bad], [(0, 0, 0), (1, 0, 0)]))

    def test_equal_key_times_a_value_over_four_wide_and_a_rotation_not_four_wide_are_refused(self):
        self.assert_refused(channel(0, "translation", [0.0, 1.0, 1.0], [(0, 0, 0)] * 3))
        self.assert_refused(channel(0, "translation", [0.0, 1.0], [(0, 0, 0, 0, 0)] * 2))
        self.assert_refused(channel(0, "rotation", [0.0, 1.0], [(0, 0, 0)] * 2))

    def test_two_tracks_with_one_name_are_refused(self):
        tracks, duration_ms = probe_tracks()
        with self.assertRaisesRegex(TracksError, "lamp/translation"):
            tracks_asset.encode(tracks + [tracks[0]], duration_ms)

    def test_a_held_cubic_channel_with_no_slope_is_one_key_with_its_three_runs(self):
        held = channel(0, "translation", [0.0, 1.0, 2.0], [(0, 0, 0), (5, 5, 5), (0, 0, 0)] * 3, "CUBICSPLINE")
        collapsed = tracks_asset.collapse_constant(held)
        self.assertEqual(collapsed["times"], [0.0])
        self.assertEqual(collapsed["values"], [(0, 0, 0), (5, 5, 5), (0, 0, 0)])

    def test_a_held_cubic_value_with_a_slope_keeps_its_keys(self):
        sloped = channel(0, "translation", [0.0, 1.0], [(0, 0, 0), (5, 5, 5), (1, 0, 0)] + [(0, 0, 0), (5, 5, 5), (0, 0, 0)],
                         "CUBICSPLINE")
        self.assertEqual(tracks_asset.collapse_constant(sloped)["times"], [0.0, 1.0])


class SamplerTests(unittest.TestCase):
    def test_a_step_track_sampled_exactly_on_a_key_is_that_key(self):
        track = {"times": [0.0, 1.0, 2.0], "values": [(1.0,), (2.0,), (3.0,)], "interpolation": "STEP",
                 "quaternion": False}
        self.assertEqual(tracks_asset.sample(track, 1.0), (2.0,))
        self.assertEqual(tracks_asset.sample(track, 0.999), (1.0,))


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.dir.name)
        (self.root / "clips").mkdir()
        (self.root / "clips" / "probe.glb").write_bytes(probe_glb())

    def tearDown(self):
        self.dir.cleanup()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def test_a_clip_file_names_the_glb_beside_it_and_its_id_is_the_stem(self):
        path = self.write("clips/walk.anim.toml", 'source = "probe.glb"\nanimation = "clip"\n')
        self.assertEqual(tracks_asset.clip_id(path), "walk")
        self.assertEqual(tracks_asset.bake(path), probe_entry())

    def test_a_clip_file_with_a_bad_key_or_a_missing_animation_is_refused(self):
        for text in ('source = "probe.glb"\n', 'source = "probe.glb"\nanimation = "clip"\nloop = true\n',
                     'source = "probe.glb"\nanimation = "clip"\nnote = "extra"\n',
                     'source = "probe.glb"\nanimation = "nope"\n', 'source = "gone.glb"\nanimation = "clip"\n'):
            path = self.write("clips/bad.anim.toml", text)
            with self.assertRaises(TracksError, msg=text):
                tracks_asset.bake(path)

    def test_a_source_that_is_not_a_glb_file_name_is_refused_by_the_rule_not_a_missing_file(self):
        # Every name below is a real, valid glb where the system can make one,
        # so only the rule can refuse it.
        (self.root / "clips" / "sub").mkdir()
        for name in ("sub/probe.glb", "probe.bin", "probe.glb.bak"):
            (self.root / "clips" / name).write_bytes(probe_glb())
        for source in ("../clips/probe.glb", "sub/probe.glb", "..\\clips\\probe.glb", "C:probe.glb",
                       str(self.root / "clips" / "probe.glb").replace("\\", "/"), "probe.bin", "probe.glb.bak"):
            path = self.write("clips/bad.anim.toml", 'source = %s\nanimation = "clip"\n' % json.dumps(source))
            with self.assertRaisesRegex(TracksError, "is not a .glb in the same folder", msg=source):
                tracks_asset.bake(path)

    def test_build_pack_finds_every_clip_file_by_searching(self):
        self.write("clips/walk.anim.toml", 'source = "probe.glb"\nanimation = "clip"\n')
        (self.root / "deeper" / "still").mkdir(parents=True)
        (self.root / "deeper" / "still" / "probe.glb").write_bytes(probe_glb())
        self.write("deeper/still/run.anim.toml", 'source = "probe.glb"\nanimation = "clip"\n')
        entries = parse_pack(build_pack.pack_bytes([self.root]))
        self.assertEqual(sorted(entries), ["run", "walk"])
        self.assertEqual(entries["walk"], (tracks_asset.TYPE, probe_entry()))

    def test_build_pack_takes_a_clip_file_named_on_its_own(self):
        path = self.write("clips/walk.anim.toml", 'source = "probe.glb"\nanimation = "clip"\n')
        self.assertEqual(sorted(parse_pack(build_pack.pack_bytes([path]))), ["walk"])

    def test_a_bad_clip_ends_build_pack_with_a_usage_error_naming_it(self):
        bad = self.write("clips/bad.anim.toml", 'source = "probe.glb"\nanimation = "nope"\n')
        with mock.patch("sys.stderr", new_callable=io.StringIO) as stderr, self.assertRaises(SystemExit) as stop:
            build_pack.main(["-o", str(self.root / "out.bin"), str(bad)])
        self.assertEqual(stop.exception.code, 2)
        self.assertIn("nope", stderr.getvalue())

    def test_two_clip_files_with_one_stem_are_refused(self):
        for folder in ("a", "b"):
            (self.root / folder).mkdir()
            (self.root / folder / "probe.glb").write_bytes(probe_glb())
            self.write(folder + "/walk.anim.toml", 'source = "probe.glb"\nanimation = "clip"\n')
        with self.assertRaises(build_pack.SettingsError):
            build_pack.pack_bytes([self.root])

    def test_every_clip_in_the_tree_packs_and_reads_back(self):
        entries = parse_pack(build_pack.pack_bytes([build_pack.DEFAULT_SEARCH]))
        clips = {name: data for name, (kind, data) in entries.items() if kind == tracks_asset.TYPE}
        self.assertGreater(len(clips), 0)
        for name, data in clips.items():
            tracks, duration_ms = tracks_asset.decode(data)
            self.assertGreater(len(tracks), 0, name)
            self.assertGreater(duration_ms, 0, name)


if __name__ == "__main__":
    unittest.main()
