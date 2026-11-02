"""Producers are found from their literal declarations; the runner makes, checks and looks up what they declare."""
import contextlib
import io
import pathlib
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "docs"))
import figures_runner  # noqa: E402
import producer  # noqa: E402

from PIL import Image  # noqa: E402

# A producer that writes one PNG per name given after --out, in its colour. Before that it runs `prologue`.
SCRIPT = '''
import os, pathlib, sys, time
from PIL import Image
Producer = dict
PRODUCER = Producer(area="{area}", needs={needs!r}, outputs={outputs!r}, run={run!r}{extra})
{prologue}
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
for name in sys.argv[3:]:
    Image.new("RGB", (2, 2), {colour!r}).save(out / name)
'''
# A prologue that fails when another run holds the tree's busy file, so two runs at once fail one.
ALONE = '''
busy = pathlib.Path(__file__).parents[2] / "busy"
busy.open("x").close()
time.sleep(0.3)
busy.unlink()
'''
# A prologue that lists data/probe.c as the C a probe compiled, as figures.build_probe does under the runner.
PROBE = f'''
(pathlib.Path(os.environ["{figures_runner.DEPENDS_FOLDER}"]) / "probe.d").write_text("probe.o: data/probe.c\\n")
'''


class Tree:
    """A checkout outside git: producers under launcher/, committed images under docs/images/."""

    def __init__(self, directory):
        self.root = pathlib.Path(directory)
        self.images = self.root / "docs/images"
        self.images.mkdir(parents=True)

    def producer(self, name, images, names, colour="red", needs=("host",), extra="", prologue=""):
        """A producer of `images` (paths under docs/images/, all in one area's folder) that writes `names`;
        `extra` adds declaration fields."""
        area = images[0].split("/")[0]
        outputs = [image.removeprefix(area + "/") for image in images]
        path = self.root / "launcher" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(SCRIPT.format(area=area, needs=needs, outputs=tuple(outputs), run=("--out", "{out}", *names),
                                      colour=colour, extra=extra, prologue=prologue))

    def write(self, name, text):
        (self.root / name).parent.mkdir(parents=True, exist_ok=True)
        (self.root / name).write_text(text)

    def commit(self, name, colour):
        (self.images / name).parent.mkdir(parents=True, exist_ok=True)
        Image.new("RGB", (2, 2), colour).save(self.images / name)

    def run(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(figures_runner, "find", lambda: producer.find(self.root)), \
                mock.patch.object(figures_runner, "ROOT", self.root), \
                mock.patch.object(figures_runner, "IMAGES", self.images), \
                mock.patch.object(figures_runner, "WORK", self.root / "work"), \
                mock.patch.object(figures_runner, "FINGERPRINTS", self.root / "fingerprints.json"), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = figures_runner.main(list(argv))
        return code, out.getvalue(), err.getvalue()


class DeclarationTests(unittest.TestCase):
    def parse(self, text):
        return producer.declaration(textwrap.dedent(text), "x.py")

    def test_a_literal_declaration_is_read_without_running_the_script(self):
        found = self.parse('''
            raise SystemExit("never imported")
            PRODUCER = Producer(area="math", needs=("host",), outputs=("a.png",), run=("--out", "{out}"))
        ''')
        self.assertEqual(found, producer.Producer("math", ("host",), ("a.png",), ("--out", "{out}")))

    def test_a_script_without_one_is_no_producer(self):
        self.assertIsNone(self.parse("print('hello')\n"))

    def test_rejected_declarations_name_the_script(self):
        for text in ('PRODUCER = Producer(area=AREA, needs=("host",), outputs=("a.png",))',
                     'PRODUCER = Producer(area="a", needs=("laser",), outputs=("a.png",))',
                     'PRODUCER = Producer(area="a", needs=("host",), outputs=("{item}.png",), each="scenes:*")',
                     'PRODUCER = Producer(area="a", needs=("host",), outputs=("a.png",), each="glob:d/*")',
                     'PRODUCER = Producer(area="a", needs=("host",), outputs=("{item}.png",))',
                     'PRODUCER = Producer(area="a", needs=("host",), outputs=("a.png",), run=("{item}",))',
                     'PRODUCER = Producer(area="a", needs=(), outputs=("a.png",))',
                     'PRODUCER = Producer(area="a", needs=("host",), outputs=())',
                     'PRODUCER = Producer(area="a", needs=("host",), outputs=("a.png",), run=("{scene}",))',
                     'PRODUCER = make_producer()'):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, "^x.py: "):
                self.parse(text)

    def test_an_image_two_producers_declare_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Tree(directory)
            tree.producer("a/one.py", ["fig/x.png"], ["x.png"])
            tree.producer("b/two.py", ["fig/x.png"], ["x.png"])
            with self.assertRaisesRegex(ValueError, "fig/x.png: made by both"):
                producer.find(tree.root)

    def test_an_image_one_producer_names_and_another_globs_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Tree(directory)
            tree.producer("a/one.py", ["fig/*.png"], ["x.png"])
            tree.producer("b/two.py", ["fig/x.png"], ["x.png"])
            with self.assertRaisesRegex(ValueError, r"made by both launcher/a/one.py \(fig/\*.png\)"):
                producer.find(tree.root)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.tree = Tree(directory.name)

    def test_build_copies_what_each_producer_declares(self):
        self.tree.producer("a/one.py", ["fig/one.png"], ["one.png"])
        self.tree.producer("b/two.py", ["fig/two-*.png"], ["two-x.png", "two-y.png"])
        code, out, _ = self.tree.run("build")
        self.assertEqual(code, 0, out)
        self.assertEqual(sorted(path.name for path in (self.tree.images / "fig").iterdir()),
                         ["one.png", "two-x.png", "two-y.png"])

    def test_check_reports_same_changed_new_and_orphan(self):
        self.tree.producer("a/one.py", ["fig/*.png"], ["same.png", "changed.png", "new.png"])
        self.tree.commit("fig/same.png", "red")
        self.tree.commit("fig/changed.png", "blue")
        self.tree.commit("fig/gone.png", "red")
        code, out, _ = self.tree.run("check")
        self.assertEqual(code, 1)
        lines = out.splitlines()
        self.assertIn("same fig/same.png", lines)
        self.assertIn("changed fig/new.png: new image", lines)
        self.assertTrue(any(line.startswith("changed fig/changed.png: frame 0") for line in lines), out)
        self.assertIn("orphan fig/gone.png: launcher/a/one.py no longer makes it", lines)

    def test_check_of_unchanged_images_exits_0(self):
        self.tree.producer("a/one.py", ["fig/one.png"], ["one.png"])
        self.tree.commit("fig/one.png", "red")
        self.assertEqual(self.tree.run("check")[0], 0)

    def test_a_missing_or_undeclared_image_fails_the_run(self):
        self.tree.producer("a/one.py", ["fig/one.png", "fig/two.png"], ["one.png", "extra.png"])
        code, _, err = self.tree.run("build")
        self.assertEqual(code, 2)
        self.assertIn("missing fig/two.png", err)
        self.assertIn("undeclared fig/extra.png", err)

    def test_a_missing_input_stops_everything_before_any_run(self):
        self.tree.producer("a/one.py", ["fig/one.png"], ["one.png"])
        script = self.tree.root / "launcher/a/one.py"
        script.write_text(script.read_text().replace('run=(', 'reads=("data/*.csv",), run=('))
        code, _, err = self.tree.run("build")
        self.assertEqual(code, 2)
        self.assertIn("reads data/*.csv, which matches no file", err)
        self.assertFalse((self.tree.root / "work").exists())

    def test_two_globs_that_make_one_image_fail_the_run(self):
        self.tree.producer("a/one.py", ["fig/x*.png"], ["xy.png"], colour="red")
        self.tree.producer("b/two.py", ["fig/*y.png"], ["xy.png"], colour="blue")
        code, _, err = self.tree.run("build")
        self.assertEqual(code, 2)
        self.assertIn("fig/xy.png: made by both launcher/a/one.py and launcher/b/two.py", err)
        self.assertFalse((self.tree.images / "fig").exists())

    def test_a_script_named_twice_runs_once(self):
        self.tree.producer("a/one.py", ["fig/one.png"], ["one.png"])
        script = str(self.tree.root / "launcher/a/one.py")
        code, out, err = self.tree.run("build", script, script, "--jobs", "2")
        self.assertEqual(code, 0, err)
        self.assertIn("wrote 1 images", out)

    def test_area_selects_its_producers_only(self):
        self.tree.producer("a/one.py", ["math/one.png"], ["one.png"])
        self.tree.producer("b/two.py", ["audio/two.png"], ["two.png"])
        code, out, _ = self.tree.run("build", "--area", "math")
        self.assertEqual(code, 0, out)
        self.assertEqual(sorted(producer.find(self.tree.root)), ["launcher/a/one.py", "launcher/b/two.py"])
        self.assertEqual([path.name for path in self.tree.images.iterdir()], ["math"])
        self.assertEqual([path.name for path in (self.tree.images / "math").iterdir()], ["one.png"])

    def test_which_names_the_producer_of_a_globbed_image(self):
        self.tree.producer("a/one.py", ["fig/two-*.png"], ["two-x.png"])
        code, out, _ = self.tree.run("which", "docs/images/fig/two-x.png", "fig/none.png")
        self.assertEqual(code, 1)
        self.assertIn("fig/two-x.png: launcher/a/one.py (area fig, needs host)", out)
        self.assertIn("fig/none.png: no producer declares it", out)

    def test_list_of_a_tree_without_producers_is_empty(self):
        self.assertEqual(self.tree.run("list", "--outputs"), (0, "", ""))

    def test_list_outputs_expands_globs_against_the_committed_images(self):
        self.tree.producer("a/one.py", ["fig/one.png", "fig/two-*.png"], ["one.png"])
        self.tree.commit("fig/two-x.png", "red")
        self.tree.commit("other/x.png", "red")
        code, out, _ = self.tree.run("list", "--outputs")
        self.assertEqual((code, out.split()), (0, ["fig/one.png", "fig/two-x.png"]))

    def test_needs_selects_the_producers_one_machine_can_run(self):
        self.tree.producer("a/one.py", ["fig/one.png"], ["one.png"])
        self.tree.producer("b/two.py", ["fig/two.png"], ["two.png"], needs=("host", "gpu"))
        code, out, err = self.tree.run("build", "--needs", "host,bakes")
        self.assertEqual(code, 0, err)
        self.assertEqual([path.name for path in (self.tree.images / "fig").iterdir()], ["one.png"])
        self.assertEqual(self.tree.run("build", "--needs", "laser")[0], 2)

    def test_each_glob_runs_once_per_matching_file(self):
        self.tree.write("data/x.csv", "1")
        self.tree.write("data/y.csv", "2")
        self.tree.producer("a/one.py", ["fig/{item}-error.png"], ["{item}-error.png"], extra=', each="glob:data/*.csv"')
        code, out, err = self.tree.run("build")
        self.assertEqual(code, 0, err)
        self.assertEqual(sorted(path.name for path in (self.tree.images / "fig").iterdir()),
                         ["x-error.png", "y-error.png"])
        self.assertIn("fig/x-error.png: launcher/a/one.py", self.tree.run("which", "fig/x-error.png")[1])

    def test_an_each_that_names_nothing_stops_everything_before_any_run(self):
        self.tree.producer("a/one.py", ["fig/{item}.png"], ["{item}.png"], extra=', each="glob:data/*.csv"')
        code, _, err = self.tree.run("build")
        self.assertEqual(code, 2)
        self.assertIn("launcher/a/one.py: each glob:data/*.csv names nothing", err)
        self.assertFalse((self.tree.root / "work").exists())

    def test_each_bakes_runs_once_per_bake_and_a_missing_bake_stops_everything(self):
        from bake import bake
        found = [mock.Mock(output="hall.fit.bundle", suffix=".bundle", key="k1"),
                 mock.Mock(output="yard.fit.bundle", suffix=".bundle", key="k2"),
                 mock.Mock(output="hall.mesh", suffix=".mesh", key="k3")]
        self.tree.producer("a/one.py", ["fig/{item}.png"], ["{item}.png"], needs=("host", "bakes"),
                           extra=', each="bakes:*.fit.bundle"')
        with mock.patch.object(figures_runner, "all_bakes", return_value=found), \
                mock.patch.object(bake, "read_lock", return_value={}), \
                mock.patch.object(bake, "fetch_all", return_value={}) as fetched:
            code, _, err = self.tree.run("build")
            self.assertEqual(code, 0, err)
            self.assertEqual(fetched.call_args[0][0], found[:2])
            fetched.side_effect = bake.BakeMissing("yard.fit.bundle: not on the release")
            code, _, err = self.tree.run("build")
        self.assertEqual(sorted(path.name for path in (self.tree.images / "fig").iterdir()),
                         ["hall.fit.png", "yard.fit.png"])
        self.assertEqual(code, 2)
        self.assertIn("yard.fit.bundle: not on the release", err)

    def test_gpu_and_board_producers_run_one_at_a_time(self):
        for name in ("a/one.py", "b/two.py", "c/three.py"):
            self.tree.producer(name, [f"fig/{pathlib.Path(name).stem}.png"], [f"{pathlib.Path(name).stem}.png"],
                               needs=("host", "gpu" if name != "c/three.py" else "board"), prologue=ALONE)
        code, _, err = self.tree.run("build", "--jobs", "4")
        self.assertEqual(code, 0, err)

    def test_changed_skips_a_run_until_its_code_reads_or_probe_c_change(self):
        self.tree.write("data/table.csv", "1")
        self.tree.write("data/probe.c", "int a;")
        self.tree.producer("a/one.py", ["fig/one.png"], ["one.png"], extra=', reads=("data/*.csv",)', prologue=PROBE)
        self.assertEqual(self.tree.run("build")[0], 0)
        for change in (None, ("data/table.csv", "2"), ("data/probe.c", "int b;"), ("launcher/a/one.py", None)):
            if change and change[1] is None:
                script = self.tree.root / change[0]
                script.write_text(script.read_text() + "\nVALUE = 1\n")
            elif change:
                self.tree.write(*change)
            with self.subTest(change=change):
                code, out, err = self.tree.run("build", "--changed")
                self.assertEqual(code, 0, err)
                self.assertEqual("unchanged launcher/a/one.py" in out, change is None, out)

    def test_check_records_only_runs_whose_images_were_same(self):
        self.tree.producer("a/one.py", ["fig/one.png"], ["one.png"])
        self.tree.commit("fig/one.png", "blue")
        self.assertEqual(self.tree.run("check", "--changed")[0], 1)
        self.assertEqual(self.tree.run("check", "--changed")[0], 1)
        self.tree.commit("fig/one.png", "red")
        self.assertEqual(self.tree.run("check", "--changed")[0], 0)
        self.assertIn("unchanged launcher/a/one.py", self.tree.run("check", "--changed")[1])

    def test_an_image_whose_item_is_gone_is_an_orphan_while_the_rest_are_unchanged(self):
        self.tree.write("data/x.csv", "1")
        self.tree.write("data/y.csv", "2")
        self.tree.producer("a/one.py", ["fig/{item}.png"], ["{item}.png"], extra=', each="glob:data/*.csv"')
        self.assertEqual(self.tree.run("build")[0], 0)
        (self.tree.root / "data/y.csv").unlink()
        code, out, _ = self.tree.run("check", "--changed")
        self.assertEqual(code, 1)
        self.assertEqual(out.splitlines(), ["unchanged launcher/a/one.py[x]",
                                            "orphan fig/y.png: launcher/a/one.py no longer makes it"])


if __name__ == "__main__":
    unittest.main()
