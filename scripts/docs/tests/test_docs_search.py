"""Tests for scripts/docs: sectioning, ranking, the vector cache and the MCP server.

Every fixture is a throwaway git repository, and the models are replaced by a
fake embedder, so nothing here downloads or starts a server. The last class
scores the real documentation lexically against eval_questions.tsv.

    python -m unittest discover -s scripts/docs/tests
"""
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from array import array
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import docs_llama  # noqa: E402
import docs_mcp  # noqa: E402
import docs_search  # noqa: E402

REPO = Path(__file__).resolve().parents[3]

GUIDE = """# Flashing Guide

Intro to flashing the board.

## Serial port

The console appears on a USB serial port.

```sh
# not a heading, a shell comment
autana monitor 30
```

## Baud rate

### Warm reset

A warm reset at high baud drops the port.

## Related

- [Other](Other.md) - why a warm reset at high baud drops the port, and
  what a warm reset does to the port at every baud.
- [Board](Board.md) - the warm reset circuit, its baud limit and the port.
"""

MEMORY = """# Memory

## Framebuffer

The framebuffer lives in PSRAM and costs 322 KiB; `gfx_init()` allocates it
in `launcher/main/gfx/gfx.c`.
"""

SCRIPT = '''#!/usr/bin/env python3
"""Bake icons from SVG sources into C arrays.

    python scripts/bake_icons.py SRC OUT

Validates every icon before it writes anything, so a bad source fails loudly.
"""
print("hi")
'''


def make_repo(files):
    root = Path(tempfile.mkdtemp())
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
    return root


def fixture():
    return make_repo({"docs/Flashing.md": GUIDE, "docs/Memory.md": MEMORY,
                      "scripts/bake_icons.py": SCRIPT, "third_party/x/README.md": GUIDE})


class Tokens(unittest.TestCase):
    def test_an_identifier_is_kept_whole_and_split(self):
        self.assertEqual(docs_search.tokens("CONFIG_LAUNCHER_SELFTEST"),
                         ["config_launcher_selftest", "config", "launcher", "selftest"])

    def test_camel_case_splits(self):
        self.assertIn("delay", docs_search.tokens("vTaskDelay"))

    def test_stopwords_drop_and_plurals_stem(self):
        self.assertEqual(docs_search.tokens("how do the materials work"), ["material", "work"])


class Sections(unittest.TestCase):
    def setUp(self):
        self.sections = docs_search.markdown_sections("docs/Flashing.md", GUIDE)

    def test_a_hash_inside_a_fence_is_not_a_heading(self):
        names = [s.headings[-1] for s in self.sections]
        self.assertNotIn("not a heading, a shell comment", names)
        serial = next(s for s in self.sections if s.headings[-1] == "Serial port")
        self.assertIn("autana monitor 30", serial.body)

    def test_a_section_carries_its_heading_path_and_line_range(self):
        warm = next(s for s in self.sections if s.headings[-1] == "Warm reset")
        self.assertEqual(warm.headings, ("Flashing Guide", "Baud rate", "Warm reset"))
        lines = GUIDE.splitlines()
        self.assertEqual(lines[warm.start - 1], "### Warm reset")
        self.assertEqual(warm.title, "Flashing Guide")

    def test_citations_are_paths_functions_and_macros(self):
        memory = docs_search.markdown_sections("docs/Memory.md", MEMORY)[-1]
        self.assertEqual(memory.cites, ("gfx_init()", "launcher/main/gfx/gfx.c"))

    def test_a_script_header_is_one_section(self):
        [section] = docs_search.script_sections("scripts/bake_icons.py", SCRIPT)
        self.assertTrue(section.body.startswith("Bake icons from SVG"))
        self.assertNotIn("print", section.body)


class Search(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.index = docs_search.Index(fixture(), semantic=False)

    def test_skipped_trees_are_not_indexed(self):
        self.assertFalse(any(s.path.startswith("third_party/") for s in self.index.sections))

    def test_the_section_that_answers_ranks_first(self):
        hits, _ = self.index.search("warm reset when flashing at high baud")
        self.assertEqual(hits[0].section.headings[-1], "Warm reset")

    def test_a_script_answers_how_to_run_it(self):
        hits, _ = self.index.search("bake icons from svg")
        self.assertEqual(hits[0].section.path, "scripts/bake_icons.py")

    def test_words_no_document_uses_are_reported(self):
        reply = docs_search.answer(self.index, "framebuffer zeppelin")
        self.assertEqual(reply["unknown"], ["zeppelin"])
        self.assertIn("no document uses: zeppelin", docs_search.format_answer(reply))

    def test_an_excerpt_stays_within_its_budget(self):
        reply = docs_search.answer(self.index, "framebuffer psram", budget=120)
        self.assertLessEqual(len(reply["results"][0]["excerpt"]), 120 * 0.5 + 4)

    def test_the_budget_holds_however_many_sections_are_excerpted(self):
        paragraph = "The palette budget is spent one shade at a time. " * 20
        root = make_repo({f"docs/Palette{n}.md": f"# Palette {n}\n\n{paragraph}\n"
                          for n in range(6)})
        index = docs_search.Index(root, semantic=False)
        reply = docs_search.answer(index, "palette budget shade", top=5, budget=400)
        used = sum(len(r["excerpt"]) for r in reply["results"])
        self.assertEqual(len(reply["results"]), 5)
        self.assertLessEqual(used, 400 + len(" ...") * 5)

    def test_a_section_is_found_by_line_or_heading(self):
        by_line = self.index.find_section("docs/Flashing.md:17")
        by_name = self.index.find_section("docs/Flashing.md#warm")
        self.assertEqual(by_line, by_name)
        self.assertEqual(by_line.headings[-1], "Warm reset")
        self.assertIsNone(self.index.find_section("docs/Nope.md:1"))

    def test_a_parent_section_lists_its_subsections_unless_deep(self):
        baud = self.index.find_section("docs/Flashing.md#baud rate")
        shallow = docs_search.format_section(self.index, baud)
        self.assertIn("Warm reset", shallow)
        self.assertNotIn("drops the port", shallow)
        self.assertIn("drops the port", docs_search.format_section(self.index, baud, deep=True))

    def test_an_outline_lists_every_heading_with_its_line(self):
        outline = docs_search.format_outline(self.index, "docs/Flashing.md")
        self.assertRegex(outline, r"\n\s+16\s+Warm reset")


def fake_embed(texts, query=False):
    """Two axes: text about memory points one way, everything else the other."""
    return [array("f", [1.0, 0.0] if any(w in t.lower() for w in ("psram", "ram", "heap"))
                  else [0.0, 1.0]) for t in texts]


class Extra(unittest.TestCase):
    """Markdown outside the tracked tree is read only when AUTANA_DOCS_EXTRA names it."""

    def setUp(self):
        self.root = make_repo({
            ".gitignore": "notes/\nvault/\nExtra.md\nNotes.txt\n",
            "docs/Flashing.md": GUIDE,
            "notes/Private.md": "# Private\n\n## Bench notes\n\nThe spare board's USB port is loose.\n",
            "notes/eval_questions.tsv": "loose usb port\tnotes/Private.md\tBench notes\n",
            "Extra.md": "# Extra\n\n## Loose ends\n\nOne more note.\n",
            "Notes.txt": "# A plain text file whose leading comment is long enough to pass for a script\n"
                         "# header, so only its suffix keeps it out of the index.\n",
        })
        self.saved = os.environ.pop(docs_search.EXTRA_ENV, None)

    def own_repository(self, name, files):
        folder = self.root / name
        for file, text in files.items():
            (folder / file).parent.mkdir(parents=True, exist_ok=True)
            (folder / file).write_text(text, encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(folder)], check=True)
        subprocess.run(["git", "-C", str(folder), "add", "."], check=True)
        return folder

    def outside(self, files):
        folder = Path(tempfile.mkdtemp())
        for file, text in files.items():
            (folder / file).write_text(text, encoding="utf-8")
        return folder

    def tearDown(self):
        os.environ.pop(docs_search.EXTRA_ENV, None)
        if self.saved is not None:
            os.environ[docs_search.EXTRA_ENV] = self.saved

    def paths(self):
        return {s.path for s in docs_search.Index(self.root, semantic=False).sections}

    def test_nothing_outside_the_tracked_tree_is_read_by_default(self):
        self.assertEqual(self.paths(), {"docs/Flashing.md"})

    def test_a_named_folder_and_file_are_read_under_their_checkout_paths(self):
        os.environ[docs_search.EXTRA_ENV] = os.pathsep.join([" notes ", "Extra.md "])
        self.assertEqual(self.paths(), {"docs/Flashing.md", "notes/Private.md", "Extra.md"})

    def test_an_extra_document_ranks_below_a_document_of_record(self):
        os.environ[docs_search.EXTRA_ENV] = "notes"
        index = docs_search.Index(self.root, semantic=False)
        extra = next(s for s in index.sections if s.path == "notes/Private.md")
        tracked = next(s for s in index.sections if s.path == "docs/Flashing.md")
        self.assertEqual(docs_search.prior(extra, index.extra), docs_search.EXTRA_PRIOR)
        self.assertEqual(docs_search.prior(tracked, index.extra), 1.0)

    def test_a_named_folder_brings_its_own_evaluation_rows(self):
        row = ("loose usb port", "notes/Private.md", "bench notes")
        self.assertNotIn(row, docs_search.load_eval(self.root))
        os.environ[docs_search.EXTRA_ENV] = "notes"
        self.assertIn(row, docs_search.load_eval(self.root))

    def test_a_folder_that_is_its_own_repository_reads_only_what_it_would_track(self):
        self.own_repository("vault", {".gitignore": "Scratch.md\n",
                                      "Bench Notes.md": "# Bench\n\n## Spare board\n\nLoose port.\n",
                                      "Scratch.md": "# Scratch\n\n## Draft\n\nNot for the index.\n"})
        os.environ[docs_search.EXTRA_ENV] = "vault"
        self.assertEqual(self.paths(), {"docs/Flashing.md", "vault/Bench Notes.md"})

    def test_an_absolute_folder_is_read_under_its_full_path(self):
        first = self.outside({"README.md": "# First\n\n## Alpha notes\n\nOne.\n"})
        second = self.outside({"README.md": "# Second\n\n## Beta notes\n\nTwo.\n"})
        os.environ[docs_search.EXTRA_ENV] = os.pathsep.join([str(first), str(second)])
        index = docs_search.Index(self.root, semantic=False)
        labels = {(first / "README.md").as_posix(), (second / "README.md").as_posix()}
        self.assertEqual({s.path for s in index.sections} - {"docs/Flashing.md"}, labels)
        self.assertEqual(index.extra, labels)

    def test_a_named_file_that_is_not_markdown_is_not_read(self):
        os.environ[docs_search.EXTRA_ENV] = "Notes.txt"
        self.assertEqual(self.paths(), {"docs/Flashing.md"})

    def test_an_edit_to_an_extra_file_mid_session_is_searchable_at_once(self):
        os.environ[docs_search.EXTRA_ENV] = "notes"
        server = docs_mcp.Server(self.root)
        ask = {"question": "zeppelin mooring"}
        with mock.patch.object(docs_llama, "installed", return_value=False):
            before, _ = server.call("docs_search", ask)
            with open(self.root / "notes" / "Private.md", "a", encoding="utf-8") as doc:
                doc.write("\n## Zeppelin mooring\n\nA zeppelin moors to the mast.\n")
            after, _ = server.call("docs_search", ask)
        self.assertIn("no document uses", before)
        self.assertIn("Zeppelin mooring", after)


class Semantic(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp()
        patches = [mock.patch.dict(os.environ, {"AUTANA_LLAMA_HOME": self.home}),
                   mock.patch.object(docs_llama, "installed", return_value=True),
                   mock.patch.object(docs_llama, "start", return_value=True),
                   mock.patch.object(docs_llama, "embed", side_effect=fake_embed)]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def test_meaning_finds_a_section_that_shares_no_word(self):
        index = docs_search.Index(fixture())
        hits, _ = index.search("how much heap is left")
        self.assertEqual(hits[0].section.path, "docs/Memory.md")
        self.assertTrue(docs_search.answer(index, "heap")["semantic"])

    def test_a_link_list_ranks_below_the_sections_it_names(self):
        def closest_is_the_link_list(texts, query=False):
            return [array("f", [1.0, 0.0] if query or "> Related" in t
                          else [0.9, 0.436] if "> Warm reset" in t else [0.0, 1.0])
                    for t in texts]

        docs_llama.embed.side_effect = closest_is_the_link_list
        index = docs_search.Index(fixture())
        hits, _ = index.search("why a warm reset drops the port at high baud", per_file=5)
        names = [(h.section.headings or (h.section.path,))[-1] for h in hits]
        self.assertLess(names.index("Warm reset"), names.index("Related"))

    def test_the_cache_embeds_a_text_once(self):
        cache = docs_llama.VectorCache()
        cache.vectors(["a psram note", "a flashing note"])
        cache.save()
        docs_llama.embed.reset_mock()
        again = docs_llama.VectorCache()
        vectors = again.vectors(["a psram note", "an edited note"])
        self.assertEqual(docs_llama.embed.call_args.args[0], ["an edited note"])
        self.assertEqual(list(vectors[0]), [1.0, 0.0])

    def test_a_server_that_fails_leaves_search_lexical(self):
        docs_llama.embed.side_effect = ConnectionRefusedError("gone")
        index = docs_search.Index(fixture())
        with mock.patch("sys.stderr", io.StringIO()):
            reply = docs_search.answer(index, "warm reset")
        self.assertFalse(reply["semantic"])
        self.assertIn("Warm reset", reply["results"][0]["heading"])

    def test_without_a_model_search_is_lexical(self):
        with mock.patch.object(docs_llama, "installed", return_value=False):
            index = docs_search.Index(fixture())
            reply = docs_search.answer(index, "framebuffer")
        self.assertFalse(reply["semantic"])
        self.assertIn("exact words only", docs_search.format_answer(reply))


class Mcp(unittest.TestCase):
    def converse(self, *messages):
        stdin = io.StringIO("".join(json.dumps(m) + "\n" for m in messages))
        stdout = io.StringIO()
        with mock.patch.object(docs_llama, "installed", return_value=False):
            docs_mcp.serve(fixture(), stdin, stdout)
        return [json.loads(line) for line in stdout.getvalue().splitlines()]

    def test_a_session_initializes_lists_and_calls(self):
        replies = self.converse(
            {"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"protocolVersion": "2025-06-18"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
             "params": {"name": "docs_search", "arguments": {"question": "warm reset baud"}}},
            {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
             "params": {"name": "docs_section", "arguments": {"ref": "docs/Nope.md:1"}}})
        self.assertEqual([r["id"] for r in replies], [1, 2, 3, 4])
        self.assertEqual(replies[0]["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual({t["name"] for t in replies[1]["result"]["tools"]},
                         {"docs_search", "docs_section", "docs_outline"})
        self.assertIn("Warm reset", replies[2]["result"]["content"][0]["text"])
        self.assertTrue(replies[3]["result"]["isError"])

    def test_an_unknown_method_is_an_error(self):
        [reply] = self.converse({"jsonrpc": "2.0", "id": 9, "method": "resources/list"})
        self.assertEqual(reply["error"]["code"], -32601)

    def test_an_unknown_protocol_version_is_answered_with_the_supported_one(self):
        [reply] = self.converse({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                 "params": {"protocolVersion": "bogus"}})
        self.assertEqual(reply["result"]["protocolVersion"], docs_mcp.PROTOCOL)

    def test_an_edit_mid_session_is_searchable_at_once(self):
        root = fixture()
        server = docs_mcp.Server(root)
        ask = {"question": "zeppelin mooring"}
        with mock.patch.object(docs_llama, "installed", return_value=False):
            before, _ = server.call("docs_search", ask)
            with open(root / "docs" / "Memory.md", "a", encoding="utf-8") as doc:
                doc.write("\n## Zeppelin mooring\n\nA zeppelin moors to the mast.\n")
            after, _ = server.call("docs_search", ask)
        self.assertIn("no document uses", before)
        self.assertIn("Zeppelin mooring", after)


class Downloads(unittest.TestCase):
    def setUp(self):
        self.target = Path(tempfile.mkdtemp()) / "model.gguf"
        self.good = __import__("hashlib").sha256(b"good").hexdigest()

    def serve(self, payload):
        return mock.patch("urllib.request.urlopen", return_value=io.BytesIO(payload))

    def test_a_mismatched_download_never_becomes_the_file(self):
        with self.serve(b"tampered"), mock.patch("sys.stderr", io.StringIO()), \
                self.assertRaises(SystemExit):
            docs_llama.download("https://example.invalid/m", self.target, self.good)
        self.assertEqual(list(self.target.parent.iterdir()), [])

    def test_a_matching_download_is_kept(self):
        with self.serve(b"good"), mock.patch("sys.stderr", io.StringIO()):
            docs_llama.download("https://example.invalid/m", self.target, self.good)
        self.assertEqual(self.target.read_bytes(), b"good")

    def test_a_verified_file_is_not_downloaded_again(self):
        self.target.write_bytes(b"good")
        with mock.patch("urllib.request.urlopen") as fetch:
            docs_llama.download("https://example.invalid/m", self.target, self.good)
        fetch.assert_not_called()


class Device(unittest.TestCase):
    LAPTOP = ("Available devices:\n"
              "  Vulkan0: AMD Radeon(TM) 610M (16268 MiB, 15455 MiB free)\n"
              "  Vulkan1: NVIDIA GeForce RTX 5080 Laptop GPU (15915 MiB, 15147 MiB free)\n")

    def listing(self, *names):
        return "Available devices:\n" + "".join(f"  Vulkan{i}: {n} (8000 MiB)\n" for i, n in enumerate(names))

    def test_a_recognised_integrated_gpu_is_left_out(self):
        self.assertEqual(docs_llama.pick_devices(self.LAPTOP), "Vulkan1")
        self.assertEqual(docs_llama.pick_devices(self.listing("NVIDIA GeForce RTX 4070", "Intel(R) UHD Graphics")), "Vulkan0")

    def test_every_other_gpu_is_kept(self):
        listing = self.listing("Intel(R) Iris(R) Xe Graphics", "NVIDIA GeForce RTX 4090", "NVIDIA GeForce RTX 3090")
        self.assertEqual(docs_llama.pick_devices(listing), "Vulkan1,Vulkan2")

    def test_nothing_recognised_leaves_the_choice_to_the_server(self):
        for listing in (self.listing("NVIDIA GeForce RTX 4070"),
                        self.listing("Intel(R) Arc(TM) 140V GPU", "NVIDIA GeForce RTX 4070"),
                        self.listing("Intel(R) UHD Graphics", "AMD Radeon(TM) Graphics")):
            self.assertIsNone(docs_llama.pick_devices(listing), listing)

    def test_llama_cpps_own_setting_wins(self):
        with mock.patch.dict(os.environ, {"LLAMA_ARG_DEVICE": "Vulkan0"}), \
                mock.patch("subprocess.run") as run:
            self.assertIsNone(docs_llama.devices())
        run.assert_not_called()

    def test_the_preset_names_the_devices_only_when_there_is_a_choice(self):
        home = tempfile.mkdtemp()
        with mock.patch.dict(os.environ, {"AUTANA_LLAMA_HOME": home}):
            with mock.patch.object(docs_llama, "devices", return_value="Vulkan1"):
                self.assertIn("device = Vulkan1", docs_llama.write_preset().read_text(encoding="utf-8"))
            with mock.patch.object(docs_llama, "devices", return_value=None):
                self.assertNotIn("device", docs_llama.write_preset().read_text(encoding="utf-8"))


class Stop(unittest.TestCase):
    def setUp(self):
        home = tempfile.mkdtemp()
        patch = mock.patch.dict(os.environ, {"AUTANA_LLAMA_HOME": home})
        patch.start()
        self.addCleanup(patch.stop)

    def test_stop_ends_only_the_server_it_started(self):
        docs_llama.pid_file().write_text("4242", encoding="ascii")
        with mock.patch("subprocess.run") as run, mock.patch("os.killpg", create=True) as kill:
            self.assertTrue(docs_llama.stop())
        if os.name == "nt":
            self.assertEqual(run.call_args.args[0], ["taskkill", "/F", "/T", "/PID", "4242"])
        else:
            self.assertEqual(kill.call_args.args[0], 4242)
        self.assertFalse(docs_llama.pid_file().exists())

    def test_a_server_it_did_not_start_is_left_alone(self):
        with mock.patch("subprocess.run") as run, mock.patch("os.killpg", create=True) as kill:
            self.assertFalse(docs_llama.stop())
        run.assert_not_called()
        kill.assert_not_called()


class RealDocuments(unittest.TestCase):
    """Exact-word retrieval must meet FLOOR on the evaluation set, whose every row names a real section.
    The engine's own documents only: a machine's AUTANA_DOCS_EXTRA must not move this gate."""
    FLOOR = 0.5

    def setUp(self):
        patch = mock.patch.dict(os.environ, {docs_search.EXTRA_ENV: ""})
        patch.start()
        self.addCleanup(patch.stop)

    def test_every_question_names_a_section_that_exists(self):
        with mock.patch.object(docs_llama, "installed", return_value=False):
            index = docs_search.Index(REPO, semantic=False)
        missing = []
        for question, path, heading in docs_search.load_eval(REPO):
            if not any(s.path == path and any(heading in name.lower()
                                              for name in s.headings + (s.title,))
                       for s in index.sections):
                missing.append(f"{path}#{heading}")
        self.assertEqual(missing, [])

    def test_lexical_recall_holds(self):
        with mock.patch.object(docs_llama, "installed", return_value=False):
            index = docs_search.Index(REPO, semantic=False)
        ranks = docs_search.evaluate(index)
        found = sum(1 for _, rank in ranks if rank)
        self.assertGreaterEqual(len(ranks), 30)
        self.assertGreaterEqual(found / len(ranks), self.FLOOR,
                                [q for q, rank in ranks if not rank])


if __name__ == "__main__":
    unittest.main()
