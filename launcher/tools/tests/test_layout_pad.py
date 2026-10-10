"""layout_pad.py: seed 0 is the plain build, any other seed picks a pad in
whole cache-line steps from the seed and the source's own name."""

import pathlib
import subprocess
import sys
import tempfile
import unittest

BUILD = pathlib.Path(__file__).resolve().parents[1] / "build"
sys.path.insert(0, str(BUILD))

import layout_pad as pads  # noqa: E402

SOURCES = [f"render/file_{n}.c" for n in range(400)]
RASTER = "esp-idf/main/libmain.a(raster.c.obj)"
OTHER = "esp-idf/vfs/libvfs.a(vfs.c.obj)"


def seeded_map(text_pad, rodata_pad, early=False):
    """A GNU ld map in the shape the firmware link writes: one object's code
    at 0x42000400 and rodata at 0x3c000400, its pads where the caller puts
    them, and with `early` a literal pool and a pooled string of the object
    placed ahead of everything."""
    lines = [".flash.text     0x42000020    0x200000",
             " *(.literal .literal.* .text .text.*)"]
    if early:
        lines += [" .literal.walk_rows", f"                0x42000000        0x8 {RASTER}"]
    lines += [f" .text.vfs_open  0x42000010        0x10 {OTHER}",
              " .text.layout_pad", f"                {text_pad:#010x}      0x2c0 {RASTER}",
              " .text.walk_rows", f"                0x42000400       0x2ef {RASTER}",
              "                0x42000400                walk_rows",
              " .text.empty    0x42000010        0x0 " + RASTER,
              ".flash.rodata   0x3c000020    0x100000"]
    if early:
        lines += [" .rodata.walk_rows.str1.4", f"                0x3c000000       0x10 {RASTER}"]
    lines += [" .rodata.layout_pad", f"                {rodata_pad:#010x}      0x400 {RASTER}",
              f" .rodata.tables 0x3c000400       0x40 {RASTER}",
              ".flash.tdata    0x3c200000        0x0"]
    return lines


class LayoutPadTest(unittest.TestCase):
    def test_seed_zero_pads_nothing(self):
        for source in SOURCES:
            self.assertEqual(pads.pad_sizes(0, source), (0, 0))

    def test_sizes_are_whole_steps_within_a_cache_way(self):
        for seed in range(1, 6):
            for source in SOURCES:
                text, rodata = pads.pad_sizes(seed, source)
                self.assertEqual(text % 32, 0)
                self.assertEqual(rodata % 64, 0)
                self.assertLessEqual(text, 4064)
                self.assertLessEqual(rodata, 8128)

    def test_every_step_is_reachable(self):
        texts = {pads.pad_sizes(seed, source)[0] for seed in range(1, 6) for source in SOURCES}
        rodatas = {pads.pad_sizes(seed, source)[1] for seed in range(1, 6) for source in SOURCES}
        self.assertEqual(len(texts), 128)
        self.assertEqual(len(rodatas), 128)

    def test_a_pad_is_repeatable_and_moves_with_the_seed(self):
        self.assertEqual(pads.pad_sizes(7, "core/job.c"), pads.pad_sizes(7, "core/job.c"))
        self.assertNotEqual([pads.pad_sizes(7, s) for s in SOURCES],
                            [pads.pad_sizes(8, s) for s in SOURCES])

    def test_sources_and_kinds_pad_independently(self):
        rows = {pads.pad_sizes(1, source) for source in SOURCES}
        self.assertGreater(len(rows), 300)
        self.assertTrue(any(text // 32 != rodata // 64 for text, rodata in rows))

    def test_the_command_line_prints_one_row_per_source(self):
        out = subprocess.run([sys.executable, str(BUILD / "layout_pad.py"), "3", "32", "4096",
                              "64", "8192", "a.c", "b/c.c"],
                             check=True, capture_output=True, text=True).stdout
        rows = [row.split(",") for row in out.split(";")]
        self.assertEqual([row[0] for row in rows], ["a.c", "b/c.c"])
        self.assertEqual([int(v) for v in rows[0][1:]], list(pads.pad_sizes(3, "a.c")))

    def test_a_wider_cache_way_allows_larger_pads(self):
        pads_seen = {pads.pad_sizes(seed, source, (32, 8192), (64, 16384))
                     for seed in range(1, 4) for source in SOURCES}
        self.assertGreater(max(text for text, _ in pads_seen), 4064)
        self.assertGreater(max(rodata for _, rodata in pads_seen), 8128)

    def test_a_pad_ahead_of_its_objects_code_and_rodata_passes(self):
        problems, count = pads.misplaced_pads(seeded_map(text_pad=0x42000020, rodata_pad=0x3c000020))
        self.assertEqual((problems, count), ([], 2))

    def test_a_code_pad_behind_its_objects_code_is_reported(self):
        problems, count = pads.misplaced_pads(seeded_map(text_pad=0x42100000, rodata_pad=0x3c000020))
        self.assertEqual(count, 2)
        self.assertEqual(len(problems), 1)
        self.assertIn("raster.c.obj", problems[0])
        self.assertIn(".text.walk_rows", problems[0])

    def test_a_rodata_pad_behind_its_objects_rodata_is_reported(self):
        problems, _ = pads.misplaced_pads(seeded_map(text_pad=0x42000020, rodata_pad=0x3c100000))
        self.assertEqual(len(problems), 1)
        self.assertIn(".rodata.tables", problems[0])

    def test_literal_pools_and_pooled_strings_do_not_count_as_the_objects_code(self):
        problems, _ = pads.misplaced_pads(seeded_map(text_pad=0x42000020, rodata_pad=0x3c000020,
                                                     early=True))
        self.assertEqual(problems, [])

    def test_objects_sharing_a_name_are_checked_only_while_every_one_is_padded(self):
        def shared(second_pad, first_pad=0x42000020):
            lines = [".flash.text     0x42000020    0x200000",
                     f" .text          0x00000000        0x0 {RASTER}",
                     f" .text          0x00000000        0x0 {RASTER}"]
            if first_pad:
                lines += [" .text.layout_pad", f"                {first_pad:#010x}      0x20 {RASTER}"]
            lines += [" .text.walk_rows", f"                0x42000400       0x2ef {RASTER}"]
            lines += [" .text.layout_pad", f"                {second_pad:#010x}      0x40 {RASTER}",
                      " .text.span_rows", f"                0x42002000       0x100 {RASTER}"]
            return lines

        self.assertEqual(pads.misplaced_pads(shared(0x42001000))[0], [])
        self.assertEqual(len(pads.misplaced_pads(shared(0x42003000, first_pad=0x42003100))[0]), 1)
        self.assertEqual(pads.misplaced_pads(shared(0x42003000, first_pad=0))[0], [])

    def test_the_check_fails_a_map_with_no_pads_or_a_misplaced_one(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / "launcher.map"
            for lines, ok in ((seeded_map(0x42000020, 0x3c000020), True),
                              (seeded_map(0x42100000, 0x3c000020), False),
                              ([line for line in seeded_map(0x42000020, 0x3c000020) if "layout_pad" not in line],
                               False)):
                path.write_text("\n".join(lines) + "\n", encoding="utf-8")
                run = subprocess.run([sys.executable, str(BUILD / "layout_pad.py"), "--check", str(path)],
                                     capture_output=True, text=True)
                self.assertEqual(run.returncode == 0, ok, run.stderr)

    def test_a_seed_that_is_not_a_number_is_refused(self):
        run = subprocess.run([sys.executable, str(BUILD / "layout_pad.py"), "x", "32", "4096", "64", "8192", "a.c"],
                             capture_output=True, text=True)
        self.assertNotEqual(run.returncode, 0)


if __name__ == "__main__":
    unittest.main()
