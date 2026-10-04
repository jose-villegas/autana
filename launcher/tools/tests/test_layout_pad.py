"""layout_pad.py: seed 0 is the plain build, any other seed picks a pad in
whole cache-line steps from the seed and the source's own name."""

import pathlib
import subprocess
import sys
import unittest

BUILD = pathlib.Path(__file__).resolve().parents[1] / "build"
sys.path.insert(0, str(BUILD))

import layout_pad as pads  # noqa: E402

SOURCES = [f"render/file_{n}.c" for n in range(400)]


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
        self.assertEqual(pads.pad_sizes(7, "util/job.c"), pads.pad_sizes(7, "util/job.c"))
        self.assertNotEqual([pads.pad_sizes(7, s) for s in SOURCES],
                            [pads.pad_sizes(8, s) for s in SOURCES])

    def test_sources_and_kinds_pad_independently(self):
        rows = {pads.pad_sizes(1, source) for source in SOURCES}
        self.assertGreater(len(rows), 300)
        self.assertTrue(any(text // 32 != rodata // 64 for text, rodata in rows))

    def test_the_command_line_prints_one_row_per_source(self):
        out = subprocess.run([sys.executable, str(BUILD / "layout_pad.py"), "3", "a.c", "b/c.c"],
                             check=True, capture_output=True, text=True).stdout
        rows = [row.split(",") for row in out.split(";")]
        self.assertEqual([row[0] for row in rows], ["a.c", "b/c.c"])
        self.assertEqual([int(v) for v in rows[0][1:]], list(pads.pad_sizes(3, "a.c")))

    def test_a_seed_that_is_not_a_number_is_refused(self):
        run = subprocess.run([sys.executable, str(BUILD / "layout_pad.py"), "x", "a.c"],
                             capture_output=True, text=True)
        self.assertNotEqual(run.returncode, 0)


if __name__ == "__main__":
    unittest.main()
