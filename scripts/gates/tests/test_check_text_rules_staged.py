"""Compressed generator inputs are retained in staged gate snapshots; a large commit's
paths reach each file-scoped gate in command lines Windows accepts."""
import sys
import unittest
import gzip
import tempfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_style_audit as audit
import check_text_rules_staged as staged


class SnapshotDataTests(unittest.TestCase):
    def test_gzip_is_binary_data_retained_for_generated_checks(self):
        self.assertIn(".gz", audit.BINARY_EXTENSIONS)
        self.assertIn(".gz", audit.DATA_EXTENSIONS)
        self.assertNotIn(".gz", staged.BINARY_EXTENSIONS - staged.DATA_EXTENSIONS)
        self.assertIn(".png", staged.BINARY_EXTENSIONS - staged.DATA_EXTENSIONS)

    def test_snapshot_keeps_gzip_bytes_for_generator_but_omits_image(self):
        capture = gzip.compress(b"timestamped generator input\n")
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def git_output(command, **kwargs):
                if command[1] == "diff":
                    return b"capture.log.gz\0"
                if command[1] == "show":
                    return b'jobs:\n  gate:\n    steps:\n      - run: python3 generated.py\n        env: {STAGED_TEXT_GATE: snapshot}\n'
                if command[1] == "ls-files":
                    return (b"100644 capture-sha 0\tcapture.log.gz\000100644 image-sha 0\timage.png\0"
                            b"100644 flow-sha 0\t.github/workflows/gate.yml\0")
                if command[1] == "rev-parse":
                    return f"{root / '.git'}\n{root / '.git/index'}\n"
                if command[1] == "cat-file":
                    self.assertEqual(b"capture-sha\nflow-sha\n", kwargs["input"])
                    return (b"capture-sha blob " + str(len(capture)).encode() + b"\n" + capture + b"\n"
                            + b"flow-sha blob 0\n\n")
                self.fail(command)
            def gate(command, **kwargs):
                snapshot = kwargs["cwd"]
                self.assertEqual(capture, (snapshot / "capture.log.gz").read_bytes())
                self.assertFalse((snapshot / "image.png").exists())
                process = mock.Mock(returncode=0)
                process.communicate.return_value = (b"", None)
                return process
            with mock.patch.object(staged, "repository", return_value=str(root)), \
                 mock.patch.object(staged.subprocess, "check_output", side_effect=git_output), \
                 mock.patch.object(staged.subprocess, "Popen", side_effect=gate):
                self.assertEqual(0, staged.main())


class PathBatchTests(unittest.TestCase):
    def test_a_thousand_staged_paths_reach_the_gate_once_each_in_short_command_lines(self):
        files = [f"launcher/main/apps/render_lab/tools/tests/fixture_{i:04d}_with_a_long_name.c" for i in range(1000)]
        seen = []
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def git_output(command, **kwargs):
                if command[1] == "diff":
                    return "\0".join(files).encode() + b"\0"
                if command[1] == "show":
                    return b'jobs:\n  gate:\n    steps:\n      - run: python3 scoped.py\n        env: {STAGED_TEXT_GATE: "true"}\n'
                if command[1] == "ls-files":
                    return b"100644 flow-sha 0\t.github/workflows/gate.yml\0"
                if command[1] == "rev-parse":
                    return f"{root / '.git'}\n{root / '.git/index'}\n"
                if command[1] == "cat-file":
                    return b"flow-sha blob 0\n\n"
                self.fail(command)
            def gate(command, **kwargs):
                # Windows' CreateProcess limit is 32767 characters for the whole line.
                self.assertLess(len(" ".join(command)), 32767)
                self.assertEqual("--paths", command[2])
                seen.extend(command[3:])
                process = mock.Mock(returncode=0)
                process.communicate.return_value = (b"", None)
                return process
            with mock.patch.object(staged, "repository", return_value=str(root)), \
                 mock.patch.object(staged.subprocess, "check_output", side_effect=git_output), \
                 mock.patch.object(staged.subprocess, "Popen", side_effect=gate):
                self.assertEqual(0, staged.main())
        self.assertEqual(files, seen)

if __name__ == "__main__":
    unittest.main()
