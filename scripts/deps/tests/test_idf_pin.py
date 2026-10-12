"""The pin's commit and esp-clang release are read from the ESP-IDF in the image, never typed."""
import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import idf_pin


def tools_json(recommended):
    return {"tools": [
        {"name": "xtensa-esp-elf", "versions": [{"name": "esp-14.2.0_20260121", "status": "recommended"}]},
        {"name": "esp-clang", "versions": [{"name": "esp-18.1.2_20240912", "status": "supported"},
                                           {"name": recommended, "status": "recommended"}]},
    ]}


class IdfPinTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.idf = pathlib.Path(self.directory.name).resolve()
        (self.idf / "tools").mkdir()
        (self.idf / "tools" / "tools.json").write_text(json.dumps(tools_json("esp-19.1.2_20250312")))
        git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
        subprocess.run([*git, "init", "-q"], cwd=self.idf, check=True)
        subprocess.run([*git, "add", "-A"], cwd=self.idf, check=True)
        subprocess.run([*git, "commit", "-qm", "idf"], cwd=self.idf, check=True)
        self.commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.idf, check=True,
                                     capture_output=True, text=True).stdout.strip()

    def tearDown(self):
        self.directory.cleanup()

    def check(self, pin):
        with contextlib.redirect_stderr(io.StringIO()) as err:
            code = idf_pin.check(pin, idf_pin.facts(self.idf))
        return code, err.getvalue()

    def test_the_recommended_esp_clang_names_its_llvm_release(self):
        self.assertEqual(idf_pin.esp_clang(self.idf / "tools" / "tools.json"), "19.1.2")

    def test_a_pin_matching_the_image_passes(self):
        self.assertEqual(self.check({"commit": self.commit, "esp_clang": "19.1.2"}), (0, ""))

    def test_each_stale_field_is_named_with_both_values(self):
        code, message = self.check({"commit": "", "esp_clang": "19.1.7"})
        self.assertEqual(code, 1)
        self.assertIn(f"records commit (none), the image has {self.commit}", message)
        self.assertIn("records esp_clang 19.1.7, the image has 19.1.2", message)
        self.assertIn("idf_pin.py write", message)

    def test_write_takes_the_image_and_reads_the_rest(self):
        path = self.idf / "pin.json"
        pin = {"image": "old", "commit": "", "image_date": "2026-01-01", "esp_clang": "1.0.0"}
        idf_pin.write(pin, idf_pin.facts(self.idf), "espressif/idf:release-v5.5@sha256:abc", "2026-10-12", path)
        self.assertEqual(json.loads(path.read_text()), {
            "image": "espressif/idf:release-v5.5@sha256:abc", "commit": self.commit,
            "image_date": "2026-10-12", "esp_clang": "19.1.2"})
        self.assertEqual(idf_pin.tag("espressif/idf:release-v5.5@sha256:abc"), "release-v5.5")

    def test_only_a_newer_release_line_is_named(self):
        tags = {"results": [{"name": n} for n in ("latest", "release-v5.5", "release-v6.1", "release-v6.0", "v6.1.1", "release-v5.10")]}
        self.assertEqual(idf_pin.newer_line("release-v5.5", tags), "release-v6.1")
        self.assertIsNone(idf_pin.newer_line("release-v6.1", tags))
        self.assertEqual(idf_pin.newer_line("release-v5.5", {"results": [{"name": "release-v5.10"}]}), "release-v5.10")


if __name__ == "__main__":
    unittest.main()
