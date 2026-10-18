"""Extraction and generated-block checks for the voice frame-time gate."""
import importlib.util
import json
import shlex
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
SCRIPT = ROOT / "launcher/tools/audio/sound_gate_tables.py"
SPEC = importlib.util.spec_from_file_location("sound_gate_tables", SCRIPT)
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class SoundGateTablesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.comparison = dict(incomplete=False, rng_seed=91, threshold=1, alpha=0.05,
                               max_seeds=4, plan=[], rows={
            "suite/z": dict(a=100, b=102, ratio=1.02, interval=[1.01, 1.04], verdict="slower"),
            "other/row": dict(a=100, b=99, ratio=0.99, interval=[0.97, 1.01], verdict="equivalent"),
            "suite/a": dict(a=100, b=99, ratio=0.99, interval=[0.98, 1.00], verdict="equivalent")})
        index = []
        for side in ("a", "b"):
            folder = self.directory / side
            folder.mkdir()
            for number in (1, 2):
                capture = folder / f"capture_{number}.log"
                capture.write_text("ms/frame avg/worst: frame 1.00/2.00"
                                   f" || sound_mix {number}.00/{number + 1}.00 ms/block n={number}\n"
                                   "unrelated || sound_mix 99.00/99.00 ms/block n=99\n")
                absent = folder / f"absent_{number}.log"
                absent.write_text("ms/frame avg/worst: frame 1.00/2.00\n")
                runs = [dict(capture=str(path), rows={"a": 100, "z": 100}, owners={}, instructions={})
                        for path in (capture, absent)]
                record = dict(seed=number, build_id=f"build-{number}", board="BOARD",
                              suites={"suite": dict(runs=runs)})
                (folder / f"flash_{number}.json").write_text(json.dumps(record))
                self.comparison["plan"].append(dict(side=side, seed=number, runs=2,
                                                    suites=[["suite", "a,z", "-"], ["other", "-", "python report.py @CAPTURE@ @TABLE@"]],
                                                    console_send=["sound stop all"] +
                                                    (["sound play tap loop"] if side == "b" else [])))
                for path in (capture, absent):
                    index.append(dict(capture_path=str(path), board="BOARD", started_at="2026-10-18T20:00:00"))
        (self.directory / "index.jsonl").write_text("\n".join(map(json.dumps, index)))
        self.save_comparison()

    def save_comparison(self):
        (self.directory / "comparison.json").write_text(json.dumps(self.comparison))

    def test_extract_weighted_mix_and_provenance(self):
        data = gate.extract(self.directory)
        self.assertEqual(data["board"], "BOARD")
        self.assertEqual(data["capture_date"], ["2026-10-18"])
        self.assertEqual(data["max_seeds"], 4)
        self.assertEqual(data["sides"]["a"]["seeds"], [1, 2])
        self.assertEqual(data["sides"]["b"]["console_send"], ["sound stop all", "sound play tap loop"])
        self.assertEqual(shlex.split(data["repeat_command"]), [
            "launcher/tools/perf/perf_compare.sh", "HEAD", "HEAD", "--max-seeds", "4",
            "--rng-seed", "91", "--threshold", "1", "--alpha", "0.05",
            "--suite", "suite", "a,z", "-", "--suite", "other", "-",
            "python report.py @CAPTURE@ @TABLE@", "--send-a", "sound stop all",
            "--send-b", "sound stop all", "--send-b", "sound play tap loop"])
        mix = data["sides"]["a"]["sound_mix"]
        self.assertAlmostEqual(mix["average_ms"], 5 / 3)
        self.assertEqual(mix["worst_ms"], 3)
        self.assertEqual(mix["blocks"], 3)
        self.assertEqual(mix["suites"], ["suite"])
        self.assertEqual(data["rows"]["suite/z"], self.comparison["rows"]["suite/z"])

    def test_incomplete_requires_override(self):
        self.comparison["incomplete"] = True
        self.save_comparison()
        with self.assertRaisesRegex(ValueError, "incomplete"):
            gate.extract(self.directory)
        self.assertTrue(gate.extract(self.directory, allow_incomplete=True)["incomplete"])

    def test_render_and_cli_drift(self):
        data_path = self.directory / "data.json"
        document = self.directory / "Sound.md"
        data_path.write_text(json.dumps(gate.extract(self.directory)))
        tables = gate.render(data_path)
        self.assertEqual(set(tables), {"sound-gate", "sound-gate-mix"})
        self.assertIn("| 3 | 2 | 1 | 0.00 | -1.00 | 2.00 | 1.00, 4.00 |", tables["sound-gate"])
        for body in tables.values():
            self.assertNotIn("suite", body.lower())
            self.assertNotIn("other", body)
        self.assertIn("| Silence | 1.67 | 3.00 | 3 |", tables["sound-gate-mix"])
        document.write_bytes("\r\n".join(f"<!-- generated: {name} -->\r\n<!-- /generated: {name} -->"
                                         for name in tables).encode())
        command = [sys.executable, str(SCRIPT), "--data", str(data_path), "--document", str(document)]
        self.assertEqual(subprocess.run(command + ["--write"], capture_output=True).returncode, 0)
        self.assertEqual(subprocess.run(command + ["--check"], capture_output=True).returncode, 0)
        original = document.read_bytes().replace(b"1.67", b"999.00")
        document.write_bytes(original)
        self.assertEqual(subprocess.run(command + ["--check"], capture_output=True).returncode, 1)
        self.assertEqual(document.read_bytes(), original)
        self.assertNotIn(b"\n", original.replace(b"\r\n", b""))


if __name__ == "__main__":
    unittest.main()
