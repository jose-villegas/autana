"""Tests for scripts/docs: sectioning, ranking, and the vector cache.

Every fixture is a throwaway plain folder - no git repository, no git binary
involved - and the models are replaced by a fake embedder, so nothing here
downloads or starts a server. The last class scores the real documentation
lexically against eval_questions.tsv.

    python -m unittest discover -s scripts/docs/tests
"""
import io
import os
import sys
import tempfile
import unittest
from array import array
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import docs_llama  # noqa: E402
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


class Corpus(unittest.TestCase):
    """corpus_files() walks the whole checkout - no VCS index consulted, no
    ignore file read - pruning only SKIPPED_PREFIXES, managed_components/
    results by name, a dotdir, a real build tree (CMakeCache.txt) or a
    fetched clone (its own .git), whatever it contains."""

    def test_root_level_and_any_depth_markdown_is_included(self):
        root = make_repo({"README.md": "# R\n", "docs/A.md": "# A\n",
                          "design/launcher/backdrop/README.md": "# Backdrop\n"})
        self.assertEqual({p for p, _ in docs_search.corpus_files(root)},
                         {"README.md", "docs/A.md", "design/launcher/backdrop/README.md"})

    def test_skipped_prefixes_are_still_excluded(self):
        root = make_repo({"docs/A.md": "# A\n",
                          "launcher/components/vendor/README.md": "# Vendor\n",
                          "third_party/x/README.md": "# Vendored\n"})
        self.assertEqual({p for p, _ in docs_search.corpus_files(root)}, {"docs/A.md"})

    def test_a_build_named_directory_that_is_not_a_build_tree_is_not_pruned(self):
        """launcher/tools/build/ is a real, tracked source folder that only shares
        the name a build directory does - only a real build tree's own marker
        file prunes a directory now, never its name."""
        root = make_repo({"launcher/tools/build/build.sh": "#!/bin/sh\n",
                          "launcher/build/README.md": "# Not a build tree either\n"})
        self.assertEqual({p for p, _ in docs_search.corpus_files(root)},
                         {"launcher/tools/build/build.sh", "launcher/build/README.md"})

    def test_a_real_build_tree_is_pruned_by_its_marker_file(self):
        root = make_repo({"launcher/build.dev/CMakeCache.txt": "# generated\n",
                          "launcher/build.dev/stray.md": "# Stray\n",
                          "docs/A.md": "# A\n"})
        self.assertEqual({p for p, _ in docs_search.corpus_files(root)}, {"docs/A.md"})

    def test_a_fetched_clone_is_pruned_by_its_own_git(self):
        """A tool pulled straight from GitHub (emsdk, say) is never named with a
        leading dot, so only checking for its own .git tells it apart from an
        ordinary source folder."""
        root = make_repo({"third_party_tools/emsdk/.git": "gitdir: ../modules/emsdk\n",
                          "third_party_tools/emsdk/README.md": "# emsdk\n",
                          "docs/A.md": "# A\n"})
        self.assertEqual({p for p, _ in docs_search.corpus_files(root)}, {"docs/A.md"})

    def test_managed_components_and_results_are_pruned_at_any_depth(self):
        root = make_repo({
            "launcher/managed_components/pkg/README.md": "# Pkg\n",
            "launcher/main/apps/sand/tools/results/README.md": "# Results\n",
            "docs/A.md": "# A\n",
        })
        self.assertEqual({p for p, _ in docs_search.corpus_files(root)}, {"docs/A.md"})

    def test_a_dotdir_is_pruned_at_any_depth(self):
        root = make_repo({"docs/.obsidian/cache.md": "# Cache\n", "docs/A.md": "# A\n"})
        self.assertEqual({p for p, _ in docs_search.corpus_files(root)}, {"docs/A.md"})


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
    """corpus_files() walks the whole checkout now, so the project's
    `docs_extra` only still matters for content outside it - inside
    self.root is always read."""

    def setUp(self):
        self.root = make_repo({"docs/Flashing.md": GUIDE})
        self.outside_root = Path(tempfile.mkdtemp())

    def set_extra(self, *entries):
        listed = ", ".join("'" + str(entry) + "'" for entry in entries)
        (self.root / "autana.local.toml").write_text(f"docs_extra = [{listed}]\n",
                                                     encoding="utf-8")

    def outside(self, files, into=None):
        folder = into or Path(tempfile.mkdtemp())
        for file, text in files.items():
            (folder / file).parent.mkdir(parents=True, exist_ok=True)
            (folder / file).write_text(text, encoding="utf-8")
        return folder

    def paths(self):
        return {s.path for s in docs_search.Index(self.root, semantic=False).sections}

    def test_nothing_outside_the_checkout_is_read_by_default(self):
        self.outside({"Private.md": "# Private\n\n## Bench notes\n\nThe spare "
                                    "board's USB port is loose.\n"}, into=self.outside_root)
        self.assertEqual(self.paths(), {"docs/Flashing.md"})

    def test_a_named_folder_outside_the_checkout_is_read_under_its_full_path(self):
        notes = self.outside({
            "Private.md": "# Private\n\n## Bench notes\n\nThe spare board's USB port is loose.\n",
        }, into=self.outside_root)
        self.set_extra(notes)
        self.assertEqual(self.paths(), {"docs/Flashing.md", (notes / "Private.md").as_posix()})

    def test_an_environment_autana_docs_extra_is_ignored(self):
        notes = self.outside({"Private.md": "# Private\n\n## Bench notes\n\nLoose port.\n"})
        with mock.patch.dict(os.environ, {"AUTANA_DOCS_EXTRA": str(notes)}):
            self.assertEqual(self.paths(), {"docs/Flashing.md"})

    def test_an_unknown_key_fails_naming_the_file_and_the_key(self):
        (self.root / "autana.local.toml").write_text("docs_extras = ['x']\n", encoding="utf-8")
        with self.assertRaises(docs_search.autana_config.ConfigError) as raised:
            self.paths()
        self.assertIn("autana.local.toml", str(raised.exception))
        self.assertIn("docs_extras", str(raised.exception))

    def test_a_named_file_outside_the_checkout_is_read_under_its_full_path(self):
        extra = self.outside({"Extra.md": "# Extra\n\n## Loose ends\n\nOne more note.\n"})
        self.set_extra(extra / "Extra.md")
        self.assertEqual(self.paths(), {"docs/Flashing.md", (extra / "Extra.md").as_posix()})

    def test_a_named_folder_inside_the_checkout_is_read_under_its_checkout_path(self):
        """`docs_extra` also accepts a checkout-relative path - redundant
        with the default whole-tree walk today, but still resolved the same way."""
        (self.root / "notes").mkdir()
        (self.root / "notes" / "Private.md").write_text(
            "# Private\n\n## Bench notes\n\nThe spare board's USB port is loose.\n",
            encoding="utf-8")
        self.set_extra("notes")
        self.assertEqual(self.paths(), {"docs/Flashing.md", "notes/Private.md"})

    def test_an_extra_document_ranks_below_a_document_of_record(self):
        notes = self.outside({"Private.md": "# Private\n\n## Bench notes\n\nThe spare "
                                            "board's USB port is loose.\n"})
        self.set_extra(notes)
        index = docs_search.Index(self.root, semantic=False)
        extra = next(s for s in index.sections if s.path == (notes / "Private.md").as_posix())
        tracked = next(s for s in index.sections if s.path == "docs/Flashing.md")
        self.assertEqual(docs_search.prior(extra, index.extra), docs_search.EXTRA_PRIOR)
        self.assertEqual(docs_search.prior(tracked, index.extra), 1.0)

    def test_a_named_folder_brings_its_own_evaluation_rows(self):
        notes = self.outside({
            "Private.md": "# Private\n\n## Bench notes\n\nThe spare board's USB port is loose.\n",
            "eval_questions.tsv": "loose usb port\tPrivate.md\tBench notes\n",
        })
        row = ("loose usb port", "Private.md", "bench notes")
        self.assertNotIn(row, docs_search.load_eval(self.root))
        self.set_extra(notes)
        self.assertIn(row, docs_search.load_eval(self.root))

    def test_a_named_folder_is_read_in_full_no_ignore_file_consulted(self):
        """`docs_extra` is a plain rglob: a folder's own .gitignore, if it has
        one, is just another file to it - not consulted, unlike the checkout walk."""
        vault = self.outside({
            ".gitignore": "Scratch.md\n",
            "Bench Notes.md": "# Bench\n\n## Spare board\n\nLoose port.\n",
            "Scratch.md": "# Scratch\n\n## Draft\n\nStill indexed.\n",
        })
        self.set_extra(vault)
        self.assertEqual(self.paths(),
                         {"docs/Flashing.md", (vault / "Bench Notes.md").as_posix(),
                          (vault / "Scratch.md").as_posix()})

    def test_an_absolute_folder_is_read_under_its_full_path(self):
        first = self.outside({"README.md": "# First\n\n## Alpha notes\n\nOne.\n"})
        second = self.outside({"README.md": "# Second\n\n## Beta notes\n\nTwo.\n"})
        self.set_extra(first, second)
        index = docs_search.Index(self.root, semantic=False)
        labels = {(first / "README.md").as_posix(), (second / "README.md").as_posix()}
        self.assertEqual({s.path for s in index.sections} - {"docs/Flashing.md"}, labels)
        self.assertEqual(index.extra, labels)

    def test_a_named_file_that_is_not_markdown_is_not_read(self):
        notes = self.outside({
            "Notes.txt": "# A plain text file whose leading comment is long enough to pass for a "
                        "script\n# header, so only its suffix keeps it out of the index.\n",
        })
        self.set_extra(notes / "Notes.txt")
        self.assertEqual(self.paths(), {"docs/Flashing.md"})

class Semantic(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp()
        patches = [mock.patch.object(docs_llama, "home", return_value=Path(self.home)),
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
        with mock.patch.object(docs_llama, "home", return_value=Path(home)):
            with mock.patch.object(docs_llama, "devices", return_value="Vulkan1"):
                self.assertIn("device = Vulkan1", docs_llama.write_preset().read_text(encoding="utf-8"))
            with mock.patch.object(docs_llama, "devices", return_value=None):
                self.assertNotIn("device", docs_llama.write_preset().read_text(encoding="utf-8"))


class LlamaSettings(unittest.TestCase):
    """The model's home and port are the project's `[docs.llama]` settings."""

    def project(self, text=None):
        folder = Path(tempfile.mkdtemp())
        if text is not None:
            (folder / "autana.local.toml").write_text(text, encoding="utf-8")
        patch = mock.patch.dict(os.environ, {"_AUTANA_PROJECT": str(folder)})
        patch.start()
        self.addCleanup(patch.stop)
        return folder

    def test_the_settings_name_the_home_and_the_port(self):
        folder = self.project("[docs.llama]\nhome = 'models'\nport = 9911\n")
        self.assertEqual(docs_llama.home(), folder / "models")
        self.assertEqual(docs_llama.port(), 9911)

    def test_without_settings_the_port_is_8765(self):
        self.project()
        self.assertEqual(docs_llama.port(), 8765)

    def test_environment_variables_no_longer_name_the_home_or_the_port(self):
        self.project()
        with mock.patch.dict(os.environ, {"AUTANA_LLAMA_HOME": "elsewhere",
                                          "AUTANA_LLAMA_PORT": "9"}):
            self.assertEqual(docs_llama.port(), 8765)
            self.assertNotEqual(docs_llama.home(), Path("elsewhere"))

    def test_a_port_that_is_not_a_number_fails_naming_the_file_and_key(self):
        self.project("[docs.llama]\nport = 'high'\n")
        with self.assertRaises(docs_llama.autana_config.ConfigError) as raised:
            docs_llama.port()
        self.assertIn("docs.llama.port", str(raised.exception))
        self.assertIn("autana.local.toml", str(raised.exception))


class Stop(unittest.TestCase):
    def setUp(self):
        home = tempfile.mkdtemp()
        patch = mock.patch.object(docs_llama, "home", return_value=Path(home))
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
    The engine's own documents only: this checkout's `docs_extra` must not move this gate."""
    FLOOR = 0.5

    def setUp(self):
        patch = mock.patch.object(docs_search, "extra_entries", return_value=[])
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
