"""Generated document blocks preserve prose and reject damaged boundaries."""
import pathlib
import sys
import tempfile
import unittest
import subprocess

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))
from generated_blocks import apply_tables, blocks, check_commands, digest, replace_block, verify


class GeneratedBlocksTests(unittest.TestCase):
    def test_writer_preserves_crlf_and_reports_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "doc.md"
            path.write_bytes(b"Intro\r\n<!-- generated: score -->\r\nold\r\n<!-- /generated: score -->\r\nEnd\r\n")
            self.assertTrue(replace_block(path, "score", "| Result |\n| 2 |\n"))
            self.assertFalse(replace_block(path, "score", "| Result |\n| 2 |\n"))
            data = path.read_bytes()
            self.assertTrue(data.startswith(b"Intro\r\n"))
            self.assertTrue(data.endswith(b"End\r\n"))
            self.assertNotIn(b"\n", data.replace(b"\r\n", b""))
            self.assertEqual(verify(data.decode()), [])

    def test_hand_edit_and_missing_hash_fail(self):
        body = "\n| 1 |\n"
        text = f"<!-- generated: score sha256={digest(body)} -->{body}<!-- /generated: score -->\n"
        self.assertEqual(verify(text), [])
        self.assertTrue(verify(text.replace("| 1 |", "| 2 |")))
        self.assertTrue(verify(text.replace(f" sha256={digest(body)}", "")))

    def test_the_writer_s_check_command_replaces_the_digest_and_regenerates_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "doc.md"
            path.write_text("<!-- generated: score -->\nold\n<!-- /generated: score -->\n", encoding="utf-8")
            self.assertTrue(replace_block(path, "score", "| 2 |\n", command="python gen.py --check"))
            text = path.read_text(encoding="utf-8")
            marker = "<!-- generated: score check: python gen.py --check -->"
            self.assertEqual(text, f"{marker}\n| 2 |\n<!-- /generated: score -->\n")
            self.assertFalse(replace_block(path, "score", "| 2 |\n", check=True, command="python gen.py --check"))
            self.assertEqual(check_commands(text), {"score": "python gen.py --check"})
            self.assertEqual(verify(text.replace("| 2 |", "| 3 |")), [])
            # A writer without a command records the digest again.
            self.assertTrue(replace_block(path, "score", "| 2 |\n"))
            self.assertEqual(check_commands(path.read_text(encoding="utf-8")), {})

    def test_invalid_markers_fail(self):
        for text in (
            "<!-- generated: a -->\n",
            "<!-- /generated: a -->\n",
            "<!-- generated: a -->\n<!-- /generated: b -->\n",
            "<!-- generated: a -->\n<!-- generated: b -->\n<!-- /generated: b -->\n<!-- /generated: a -->\n",
            "<!-- generated: a bad-hash -->\n<!-- /generated: a -->\n",
            "<!-- generated: a check: -->\n<!-- /generated: a -->\n",
            "<!-- generated: a -->\n<!-- /generated: a check: python gen.py -->\n",
            "<!-- generated: a -->\n<!-- /generated: a -->\n" * 2,
        ):
            with self.subTest(text=text), self.assertRaises(ValueError):
                blocks(text)

    def test_missing_block_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "doc.md"
            path.write_text("Prose\n")
            with self.assertRaises(ValueError):
                replace_block(path, "absent", "table\n")

    def test_prose_bytes_survive_mixed_line_endings(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "doc.md"
            path.write_bytes(b"Intro\n<!-- generated: score -->\r\nold\r\n<!-- /generated: score -->\r\nEnd\n")
            replace_block(path, "score", "table\n")
            self.assertTrue(path.read_bytes().startswith(b"Intro\n"))
            self.assertTrue(path.read_bytes().endswith(b"\r\nEnd\n"))

    def test_table_tree_check_is_read_only_and_finds_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            subprocess.run(["git", "init", "-q", directory], check=True)
            doc = root / "measured results.md"
            doc.write_text("<!-- generated: scores -->\n<!-- /generated: scores -->\n", encoding="utf-8")
            subprocess.run(["git", "add", doc.name], cwd=root, check=True)
            tables = root / "out/tables"
            tables.mkdir(parents=True)
            (tables / "scores.md").write_text("| 2 |\n", encoding="utf-8")
            before = doc.read_bytes()
            self.assertTrue(apply_tables(root, tables, check=True))
            self.assertEqual(doc.read_bytes(), before)
            self.assertTrue(apply_tables(root, tables))
            self.assertFalse(apply_tables(root, tables, check=True))
            (tables / "unknown.md").write_text("table\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                apply_tables(root, tables)
