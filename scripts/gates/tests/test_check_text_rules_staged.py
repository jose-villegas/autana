"""Compressed generator inputs are retained in staged gate snapshots; a large commit's
paths reach each file-scoped gate in command lines Windows accepts."""
import subprocess
import sys
import unittest
import gzip
import tempfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_style_audit as audit
import check_text_rules_staged as staged


def run_staged(test, staged_files, workflow_env, entries, blobs, gate, cat_input=None):
    """Run the staged gates over a fake index: one workflow step with `workflow_env`, `entries` as
    `ls-files -s` lines, `blobs` as the `cat-file --batch` answer; `gate` stands in for each gate."""
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        answers = {
            "diff": b"".join(name.encode() + b"\0" for name in staged_files),
            "show": (b"jobs:\n  gate:\n    steps:\n      - run: python3 gate.py\n        env: {STAGED_TEXT_GATE: "
                     + workflow_env + b"}\n"),
            "ls-files": entries + b"100644 flow-sha 0\t.github/workflows/gate.yml\0",
            "rev-parse": f"{root / '.git'}\n{root / '.git/index'}\n",
            "cat-file": blobs + b"flow-sha blob 0\n\n",
        }
        def git_output(command, **kwargs):
            if command[1] == "cat-file" and cat_input is not None:
                test.assertEqual(cat_input, kwargs["input"])
            if command[1] not in answers:
                test.fail(command)
            return answers[command[1]]
        def finished(command, **kwargs):
            gate(command, **kwargs)
            process = mock.Mock(returncode=0)
            process.communicate.return_value = (b"", None)
            return process
        with mock.patch.object(staged, "repository", return_value=str(root)), \
             mock.patch.object(staged.subprocess, "check_output", side_effect=git_output), \
             mock.patch.object(staged.subprocess, "Popen", side_effect=finished):
            test.assertEqual(0, staged.main())


class SnapshotDataTests(unittest.TestCase):
    def test_gzip_is_binary_data_retained_for_generated_checks(self):
        self.assertIn(".gz", audit.BINARY_EXTENSIONS)
        self.assertIn(".gz", audit.DATA_EXTENSIONS)
        self.assertNotIn(".gz", staged.BINARY_EXTENSIONS - staged.DATA_EXTENSIONS)
        self.assertIn(".png", staged.BINARY_EXTENSIONS - staged.DATA_EXTENSIONS)

    def test_snapshot_keeps_gzip_bytes_for_generator_but_omits_image(self):
        capture = gzip.compress(b"timestamped generator input\n")
        def gate(command, **kwargs):
            snapshot = kwargs["cwd"]
            self.assertEqual(capture, (snapshot / "capture.log.gz").read_bytes())
            self.assertFalse((snapshot / "image.png").exists())
        run_staged(self, ["capture.log.gz"], b"snapshot",
                   b"100644 capture-sha 0\tcapture.log.gz\0" b"100644 image-sha 0\timage.png\0",
                   b"capture-sha blob " + str(len(capture)).encode() + b"\n" + capture + b"\n", gate,
                   cat_input=b"capture-sha\nflow-sha\n")


class PathBatchTests(unittest.TestCase):
    def test_a_thousand_staged_paths_reach_the_gate_once_each_in_short_command_lines(self):
        # Every other path has a space, which Windows quoting widens by two.
        files = [f"launcher/main/apps/render_lab/tools/tests/fixture {i:04d}_with_a_long_name.c" if i % 2 else
                 f"launcher/main/apps/render_lab/tools/tests/fixture_{i:04d}_with_a_long_name.c" for i in range(1000)]
        seen = []
        def gate(command, **kwargs):
            # Windows' CreateProcess limit is 32767 characters for the whole line, as it quotes it.
            self.assertLess(len(subprocess.list2cmdline(command)), 32767)
            self.assertEqual("--paths", command[2])
            seen.extend(command[3:])
        run_staged(self, files, b'"true"', b"", b"", gate)
        self.assertEqual(files, seen)


if __name__ == "__main__":
    unittest.main()
