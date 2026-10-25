"""The package dependency diagram in Layers.md is written from each package's declared requires,
and the checked-in block matches it."""
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "launcher/tools/build"))
from package_requires import BLOCK, DOC, diagram


class PackageRequiresDiagram(unittest.TestCase):
    def test_one_node_per_package_and_one_arrow_per_declared_dependency(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            for name, text in (("a", "set(a_requires b c)\n"), ("b", "set(b_requires c)\n"), ("c", "\n")):
                (root / name).mkdir()
                (root / name / "CMakeLists.txt").write_text(text, encoding="utf-8")
            lines = diagram(root).splitlines()
            self.assertEqual(["a --> b", "a --> c", "b --> c"], [line.strip() for line in lines if "-->" in line])
            self.assertEqual(3, sum("[" in line for line in lines))

    def test_the_checked_in_diagram_is_current(self):
        text = (REPO / DOC).read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertIn(diagram(REPO / "launcher/packages"), text)
        self.assertIn(f"generated: {BLOCK} check:", text)


if __name__ == "__main__":
    unittest.main()
