"""Generated document blocks preserve prose and reject damaged boundaries."""
import pathlib
import sys
import tempfile
import unittest
import subprocess

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))
from generated_blocks import apply_tables, blocks, check_commands, digest, reference_jumps, replace_block, verify

GENERATOR = pathlib.Path(__file__).resolve().parents[1] / "render/generated_blocks.py"


def scores_tree(directory, published, refreshed):
    """A git tree in `directory` whose Fidelity.md block `scores` holds
    `published`, and a tables folder whose scores.md holds `refreshed`: (root,
    doc, tables)."""
    root = pathlib.Path(directory)
    subprocess.run(["git", "init", "-q", directory], check=True)
    doc = root / "Fidelity.md"
    doc.write_text(f"<!-- generated: scores -->\n{published}<!-- /generated: scores -->\n", encoding="utf-8")
    subprocess.run(["git", "add", doc.name], cwd=root, check=True)
    tables = root / "out/tables"
    tables.mkdir(parents=True)
    (tables / "scores.md").write_text(refreshed, encoding="utf-8")
    return root, doc, tables


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

    def test_a_refreshed_mean_de_jump_is_refused_unless_accepted(self):
        # #1597 drew every Sponza figure from a still camera: each mean dE76 rose from 5-13 to 31-38.
        header = "| Variant | Mean dE76 | p95 dE76 |\n|---|---:|---:|\n"
        published = header + "| fitted | 5.468 | 15.642 |\n| flat | 13.149 | 46.465 |\n"
        cases = {"doubled and over +5": ("| fitted | 34.604 | 65.969 |\n| flat | 13.0 | 46.0 |\n", True),
                 "a new row": ("| fitted | 5.468 | 15.642 |\n| flat | 13.149 | 46.465 |\n| new | 90 | 99 |\n", False),
                 "doubled, under +5": ("| fitted | 9.9 | 15.642 |\n| flat | 13.149 | 46.465 |\n", False),
                 "over +5, not doubled": ("| fitted | 10.5 | 15.642 |\n| flat | 19.0 | 46.465 |\n", False),
                 "exactly doubled and +5": ("| fitted | 5.468 | 15.642 |\n| flat | 13.149 | 46.465 |\n"
                                            .replace("5.468", "10.936"), False),
                 "only p95 jumps": ("| fitted | 5.468 | 99.0 |\n| flat | 13.149 | 46.465 |\n", False)}
        for case, (rows, refused) in cases.items():
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                root, _doc, tables = scores_tree(directory, published, header + rows)
                if refused:
                    with self.assertRaisesRegex(ValueError, r"Fidelity\.md#scores fitted: Mean dE76 5\.468 -> 34\.604"):
                        apply_tables(root, tables, check=True)
                    self.assertTrue(apply_tables(root, tables, accept_jumps=True))
                else:
                    self.assertTrue(apply_tables(root, tables, check=True))

    def test_a_rank_shared_by_two_tables_is_compared_within_its_own(self):
        # Object-error blocks restart their ranks in each table; a jump in the
        # first table must not hide behind the second's same rank.
        header = "| Rank | Object part | Mean dE76 |\n|---|---|---:|\n"
        published = f"{header}| 1 | a | 6.7 |\n\n{header}| 1 | b | 4.0 |\n"
        refreshed = f"{header}| 1 | a | 999 |\n\n{header}| 1 | b | 4.0 |\n"
        self.assertEqual(reference_jumps(published, refreshed), ["1: Mean dE76 6.7 -> 999"])

    def test_the_indirect_look_columns_are_reference_scores_too(self):
        header = "| Look | Mean dE76, physical | Mean dE76, own |\n|---|---:|---:|\n"
        self.assertEqual(reference_jumps(header + "| baked | 4.0 | 3.0 |\n", header + "| baked | 30.0 | 3.0 |\n"),
                         ["baked: Mean dE76, physical 4.0 -> 30.0"])

    def test_the_command_line_refuses_a_jump_until_accepted(self):
        header = "| Variant | Mean dE76 |\n|---|---:|\n"
        with tempfile.TemporaryDirectory() as directory:
            _root, doc, tables = scores_tree(directory, header + "| fitted | 5.5 |\n", header + "| fitted | 34.6 |\n")
            command = [sys.executable, str(GENERATOR), "--root", directory, "--tables", str(tables)]
            refused = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(refused.returncode, 2)
            self.assertIn("--accept-reference-jump", refused.stderr)
            self.assertNotIn("34.6", doc.read_text(encoding="utf-8"))
            accepted = subprocess.run(command + ["--accept-reference-jump"], capture_output=True, text=True)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            self.assertIn("34.6", doc.read_text(encoding="utf-8"))

    def test_a_table_links_from_its_owning_pages_folder(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            subprocess.run(["git", "init", "-q", directory], check=True)
            pages = {"docs/area/Shallow.md": "shallow", "docs/area/topic/Deep.md": "deep"}
            for page, block in pages.items():
                (root / page).parent.mkdir(parents=True, exist_ok=True)
                (root / page).write_text(f"<!-- generated: {block} -->\n<!-- /generated: {block} -->\n",
                                         encoding="utf-8")
                subprocess.run(["git", "add", page], cwd=root, check=True)
            tables = root / "out/tables"
            tables.mkdir(parents=True)
            for block in pages.values():
                (tables / f"{block}.md").write_text("![chart](../images/c.png) [web](https://x.org/a) [here](#a)\n",
                                                   encoding="utf-8")
            apply_tables(root, tables)
            self.assertIn("](../images/c.png) [web](https://x.org/a) [here](#a)",
                          (root / "docs/area/Shallow.md").read_text(encoding="utf-8"))
            self.assertIn("](../../images/c.png) [web](https://x.org/a) [here](#a)",
                          (root / "docs/area/topic/Deep.md").read_text(encoding="utf-8"))
