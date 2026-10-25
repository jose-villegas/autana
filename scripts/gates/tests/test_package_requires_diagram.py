"""The package dependency diagram in Layers.md is written from each package's declared requires,
and the node ids survive a package named like a Mermaid keyword."""
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "launcher/tools/build"))
from package_requires import diagram


class PackageRequiresDiagram(unittest.TestCase):
    def test_one_node_per_package_and_one_arrow_per_declared_dependency(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            for name, text in (("end", "set(end_requires b c)\n"), ("b", "set(b_requires c)\n"), ("c", "\n")):
                (root / "packages" / name / "include").mkdir(parents=True)
                (root / "packages" / name / "CMakeLists.txt").write_text(text, encoding="utf-8")
            lines = diagram(root).splitlines()
            self.assertEqual(["p_b --> p_c", "p_end --> p_b", "p_end --> p_c"], [line.strip() for line in lines if "-->" in line])
            self.assertEqual(3, sum("[" in line for line in lines))


if __name__ == "__main__":
    unittest.main()
