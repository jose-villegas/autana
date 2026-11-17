"""The model read from a tree, the LikeC4 text written from it, the project-file
command, and the installed LikeC4 accepting what is written."""
import importlib.util
import json
import pathlib
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
import build  # noqa: E402
import likec4  # noqa: E402
import model  # noqa: E402

spec = importlib.util.spec_from_file_location('architecture_site', HERE / 'site.py')
site = importlib.util.module_from_spec(spec)
spec.loader.exec_module(site)

C = (('.h',), ('.c',))


def write_tree(case, files):
    """A temporary folder holding `files` (path -> text), removed after the test."""
    temp = tempfile.mkdtemp()
    case.addCleanup(shutil.rmtree, temp, ignore_errors=True)
    for rel, text in files.items():
        path = pathlib.Path(temp) / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
    return model.Tree(temp)


def git(root, *args):
    return subprocess.run(['git', '-C', str(root), '-c', 'user.name=t', '-c', 'user.email=t@t', *args],
                          capture_output=True, text=True, check=True).stdout.strip()


def fixture(names=('a', 'b', 'c')):
    """Model data for a drill container `firmware` holding one component per
    name, each with one module, as a caller with build images would give it."""
    system = dict(id='autana', name='Fixture', type='system', desc='System', src='README.md')
    return dict(system=system, source=dict(repo='https://github.com/example/repo', commit='abc'),
                context=dict(nodes=[system], edges=[]),
                containers=dict(nodes=[dict(id='firmware', name='Firmware', type='container',
                                           desc='Image', src='launcher/main/main.c', tech='C', drill=True)], edges=[]),
                components=[dict(id=n, parent=n.rpartition('/')[0], folder='launcher/main/' + n,
                                 desc=n, src='launcher/main/' + n + '/README.md')
                            for n in sorted({p for n in names for p in ('/'.join(n.split('/')[:i]) for i in range(1, len(n.split('/')) + 1))})],
                files=[dict(path='launcher/main/' + n + '/icon.c', comp=n, kind='src',
                            lines=7, summary='Draws icons.', inc=[], ext=[]) for n in names],
                tags=dict(app='green', engine='blue', iram='red', dev='amber', diag='secondary', devcode=None),
                roles=('app', 'engine'), release_hidden=('dev', 'diag'))


def edge(text, a, b):
    return next(line for line in text.splitlines() if f'{a} -[includes]-> {b} ' in line)


def validate(case, text):
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        (root / 'likec4.config.json').write_text('{"name": "fixture"}')
        (root / 'fixture.c4').write_text(text, encoding='utf-8')
        site.validate(root, {'name': 'fixture'})


class Docs(unittest.TestCase):
    def best(self, docs, files):
        return model.assign_docs(docs, [], [dict(path=p, comp='fixture', module='target')
                                           for p in files]).get('target')

    def docs(self):
        files = {
            'docs/Linear.md': '# Linear!\n\nVectors and rotations.\n\n## API & types\n\n'
                              'Maps and transforms.\n\n#include <math/linear/x.h>\n'
                              '## API & types\n\n#include <math/linear/x.h>\n',
            'docs/Catalogue.md': '# Catalogue\n\n[x](../pkg/include/math/linear/x.h) `math/linear/x.h`\n'
                                 '`math/scalar/y.h` `math/motion/z.h`\n',
            'docs/Citations.md': '# Citations\n\n| Paper | Source |\n|---|---|\n'
                                 '| Maps | `math/linear/x.h` |\n| Numbers | `math/scalar/y.h` |\n'
                                 '| Motion | `math/motion/z.h` |\n',
            'docs/Elsewhere.md': '# Other folder\n\n`math/scalar/y.h` `math/scalar/y.h` `math/linear/x.h`\n',
        }
        self.tree = write_tree(self, files)
        tracked = set(files) | {'pkg/include/math/linear/x.h', 'pkg/include/math/scalar/y.h', 'pkg/include/math/motion/z.h'}
        return tracked, model.doc_references(self.tree, tracked, ('pkg/include',), C[0] + C[1])

    def test_folder_falls_back_to_the_matched_section_paragraph(self):
        tracked, docs = self.docs()
        self.assertEqual(model.describe(self.tree, 'pkg/include/math/linear', tracked, *C, False,
                                        self.best(docs, {'pkg/include/math/linear/x.h'})),
                         ('Maps and transforms.', 'docs/Linear.md'))

    def test_doc_links_count_resolved_references_and_slug_repeated_headings(self):
        _, docs = self.docs()
        best = self.best(docs, {'pkg/include/math/linear/x.h'})
        self.assertEqual((best['path'], best['anchor'], best['heading']), ('docs/Linear.md', 'api--types', 'API & types'))
        self.assertEqual(docs[-1]['anchor'], 'api--types-1')

    def test_sections_need_a_strict_majority_of_references(self):
        tracked, docs = self.docs()
        files = {'pkg/include/math/linear/x.h'}
        catalogue = [d for d in docs if d['path'] == 'docs/Catalogue.md']
        self.assertIsNone(self.best(catalogue, files))
        for path in ('docs/Citations.md', 'docs/Elsewhere.md'):
            with self.subTest(path=path):
                self.assertIsNone(self.best([d for d in docs if d['path'] == path], files))
        self.assertEqual(model.describe(self.tree, 'pkg/include/math/linear', tracked, *C, False, self.best(catalogue, files)),
                         ('', 'pkg/include/math/linear'))

    def test_doc_ranking_prefers_subtree_count_then_share_path_and_occurrence(self):
        sections = [dict(path=path, refs=refs) for path, refs in [
            ('docs/A.md', ['x']), ('docs/B.md', ['x', 'x', 'y']),
            ('docs/C.md', ['x', 'x']), ('docs/B.md', ['x', 'x']),
            ('docs/B.md', ['x', 'x'])]]
        self.assertIs(self.best(sections, {'x'}), sections[3])
        broader = dict(path='docs/Z.md', refs=['x', 'x', 'x', 'y', 'y'])
        self.assertIs(self.best(sections + [broader], {'x'}), broader)

    def test_parent_does_not_take_a_childs_section(self):
        data = fixture(('apps/sand',))
        path = data['files'][0]['path']
        section = dict(path='docs/Sand.md', anchor='gravity', heading='Gravity',
                       desc='Sand gravity.', refs=[path])
        links = model.assign_docs([section], data['components'], data['files'])
        self.assertNotIn('apps', links)
        self.assertIs(links['apps/sand'], section)
        self.assertIs(links[path.rsplit('.', 1)[0]], section)
        data['doc_links'] = links
        self.assertEqual(likec4.emit(data).count('Doc: Gravity'), 2)
        self.assertEqual(model.describe(write_tree(self, {}), 'launcher/main/apps', {path}, *C, False, links.get('apps')),
                         ('', 'launcher/main/apps'))

    def test_component_without_own_section_takes_its_direct_modules_section(self):
        data = fixture(('math/scalar',))
        path = data['files'][0]['path']
        section = dict(path='docs/Scalar.md', anchor='integers', heading='Integers',
                       desc='Integer arithmetic.', refs=[path])
        links = model.assign_docs([section], data['components'], data['files'])
        self.assertIs(links[path.rsplit('.', 1)[0]], section)
        self.assertIs(links['math/scalar'], section)
        self.assertNotIn('math', links)
        self.assertEqual(model.describe(write_tree(self, {}), 'launcher/main/math/scalar', {path}, *C, False,
                                        links['math/scalar']), ('Integer arithmetic.', 'docs/Scalar.md'))

    def test_component_owns_a_section_spread_over_its_modules(self):
        data = fixture(('apps/sand',))
        first = data['files'][0]
        data['files'].append(dict(first, path='launcher/main/apps/sand/gravity.c'))
        section = dict(path='docs/Sand.md', anchor='gravity', heading='Gravity',
                       desc='Sand gravity.', refs=[f['path'] for f in data['files']])
        links = model.assign_docs([section], data['components'], data['files'])
        self.assertEqual(links, {'apps/sand': section})
        data['doc_links'] = links
        text = likec4.emit(data)
        self.assertEqual(text.count("Doc: Gravity"), 1)
        self.assertIn("/docs/Sand.md#gravity 'Doc: Gravity'", text)

    def test_section_spread_over_children_belongs_to_the_folder(self):
        data = fixture(('apps/sand', 'apps/other'))
        section = dict(path='docs/Apps.md', refs=[f['path'] for f in data['files']])
        self.assertEqual(model.assign_docs([section], data['components'], data['files']), {'apps': section})

    def test_bare_source_names_resolve_only_when_unique(self):
        tracked, _ = self.docs()
        (self.tree.root / 'docs/Bare.md').write_text('# Bare\n\n`x.h` `y.h`\n#include <x.h>\n#include "y.h"\n', encoding='utf8')
        tracked.update({'docs/Bare.md', 'other/y.h'})
        docs = model.doc_references(self.tree, tracked, ('pkg/include', 'pkg/include/math/scalar'), C[0] + C[1])
        section = next(d for d in docs if d['path'] == 'docs/Bare.md')
        self.assertEqual(section['refs'], ['pkg/include/math/linear/x.h'] * 2)


class PublishedCommit(unittest.TestCase):
    """Links point at a commit the remote has, not at a merge made locally."""

    def repo(self):
        tree = write_tree(self, {})
        run = lambda *a: git(tree.root, *a)  # noqa: E731
        run("init", "-q", "-b", "main")
        run("commit", "-q", "--allow-empty", "-m", "base")
        return tree, run

    def test_a_merge_made_only_here_links_to_the_remote_main_it_merged(self):
        tree, run = self.repo()
        run("checkout", "-q", "-b", "pages")
        run("commit", "-q", "--allow-empty", "-m", "pages only")
        run("checkout", "-q", "main")
        run("commit", "-q", "--allow-empty", "-m", "main moves")
        main = run("rev-parse", "HEAD")
        run("update-ref", "refs/remotes/origin/main", main)
        run("checkout", "-q", "pages")
        run("merge", "-q", "--no-edit", "main")
        self.assertNotEqual(run("rev-parse", "HEAD"), main)
        self.assertEqual(tree.published_commit(), main)

    def test_without_a_remote_main_the_checkout_itself_is_linked(self):
        tree, run = self.repo()
        self.assertEqual(tree.published_commit(), run("rev-parse", "HEAD"))


class Words(unittest.TestCase):
    def test_summary_and_description_share_the_full_opening(self):
        text = '/* pixels: draws pixels. Keeps their colours. */'
        self.assertEqual(model.opening('x.c', text), 'Draws pixels. Keeps their colours.')
        self.assertEqual(model.summary('x.c', text), 'Draws pixels.')
        self.assertLessEqual(len(model.summary('x.c', '/* ' + 'word ' * 80 + '*/')), model.SUMMARY_LENGTH)

    def test_the_first_sentence_of_the_top_comment_without_its_name_lead(self):
        cases = (
            ("/* gfx: the one framebuffer. Drawing lives here. */\n", "The one framebuffer."),
            ("/*\n * job_queue (core 1) - work handed across. More.\n */\n", "Work handed across."),
            ("// tween: easing in one place.\n// Second line.\nint x;\n", "Easing in one place."),
            ("#pragma once\n/* upscale: maps. More. */\n", "Maps."),
            ("/* ======\n * GENERATED FILE - do not edit.\n */\n", "Generated file."),
            ("#include <stdio.h>\n/* not a top comment */\n", ""),
            ("/* " + "word " * 80 + "end. */", None),
        )
        for text, expected in cases:
            with self.subTest(text=text[:30]):
                got = model.summary("m/x.c", text)
                if expected is None:
                    self.assertLessEqual(len(got), model.SUMMARY_LENGTH)
                    self.assertTrue(got.endswith("..."))
                else:
                    self.assertEqual(got, expected)

    def test_a_suite_with_no_header_is_described_by_the_tests_it_runs(self):
        text = "#include \"unity.h\"\nvoid\nrun(void) {\n    RUN_TEST(test_a_take_is_aligned);\n    RUN_TEST(test_rewind_frees);\n}\n"
        self.assertEqual(model.summary("t/suite_arena.c", text), "A take is aligned; rewind frees.")
        self.assertEqual(model.summary("t/arena.c", text), "")

    def test_a_readme_paragraph_skips_headings_badges_and_html(self):
        text = "# Name\n\n[![b](x)](y)\n\n<table>\n</table>\n\nIt is **a** [thing](t.md)\nthat runs.\n\nMore.\n"
        self.assertEqual(model.paragraph(text), "It is a thing that runs.")

    def test_a_file_opens_with_its_first_sentence_whatever_its_language(self):
        cases = (
            ("a/README.md", "# A\n\n[![b](x)](y)\n\nIt is **a** tool.\nIt edits.\n", "It is a tool. It edits."),
            ("a/x.py", '#!/usr/bin/env python3\n"""x: does one thing. Then more."""\n', "Does one thing. Then more."),
            ("a/x.py", '"""Builds a page.\n\n    usage\n"""\n', "Builds a page."),
            ("a/x.sh", "#!/bin/sh\n#\n# x - runs the suites. More.\n#\ncode\n", "Runs the suites. More."),
            ("a/x", "#!/usr/bin/env bash\n#\n# autana - the command.\nhere=1\n", "The command."),
            ("a/x.c", "/* x: the thing. More. */\n", "The thing. More."),
            ("a/x.hpp", "// The window's panels. They record.\n#pragma once\n", "The window's panels. They record."),
            ("a/x.cpp", "#include \"x.hpp\"\n", ""),
            ("a/x.sh", "#!/bin/sh\nset -e\n", ""),
        )
        for rel, text, expected in cases:
            with self.subTest(rel=rel, text=text[:24]):
                self.assertEqual(model.opening(rel, text), expected)

    def test_tests_and_tools_are_told_apart_by_where_they_sit_and_what_they_are_called(self):
        cases = (
            ("launcher/main/apps/sand/tests/suite_sand_x.c", "test"),
            ("launcher/main/apps/sand/app_sand_test.h", "test"),
            ("launcher/test/suites/suite_gfx.c", "test"),
            ("launcher/main/render/suite_inline.c", "test"),
            ("tests/view_test.cpp", "test"),
            ("launcher/main/apps/sand/tools/sand_sim_render_host.c", "tool"),
            ("launcher/main/apps/sand/sand.h", "src"),
            ("src/window/panels.cpp", "src"),
        )
        for rel, expected in cases:
            with self.subTest(rel=rel):
                self.assertEqual(model.kind(rel), expected)


class Components(unittest.TestCase):
    def test_every_folder_nests_without_readmes(self):
        paths = ["launcher/main/main.c", "launcher/main/gfx/gfx.c",
                 "launcher/main/gfx/present/send/worker.c"]
        folders, owners, parents = model.components_of(paths, ("launcher/main",), C[0])
        self.assertEqual(owners[paths[0]], "main")
        self.assertEqual(owners[paths[-1]], "gfx/present/send")
        self.assertEqual(parents, {"main": "", "gfx": "", "gfx/present": "gfx",
                                   "gfx/present/send": "gfx/present"})
        self.assertEqual(folders["gfx/present"], "launcher/main/gfx/present")
        self.assertNotIn("gfx/present", owners.values())

    def test_package_merges_public_headers_and_sources(self):
        root = "launcher/packages/pkg"
        paths = [root + p for p in ("/include/pkg/a/x.h", "/include/pkg/b/y.h", "/src/a/x.c")]
        folders, owners, parents = model.components_of(paths, (root,), C[0], {root: "pkg"}, (root + "/include",))
        self.assertEqual(set(folders), {"pkg", "pkg/a", "pkg/b"})
        self.assertEqual(owners[paths[0]], owners[paths[2]])
        self.assertEqual(parents["pkg/a"], "pkg")
        self.assertEqual(folders["pkg/a"], root + "/include/pkg/a")

    def test_component_id_collision_names_both_folders(self):
        with self.assertRaisesRegex(model.Undescribed, "one/x.*two/x|two/x.*one/x"):
            model.components_of(["one/x/a.c", "two/x/b.c"], ("one", "two"), C[0])


class Describe(unittest.TestCase):
    def tree(self, files):
        self.fs = write_tree(self, files)
        return set(files)

    def describe(self, folder, tracked, *args, suffixes=C, **kwargs):
        return model.describe(self.fs, folder, tracked, *suffixes, *args, **kwargs)

    def test_the_source_named_after_the_folder_wins_inside_then_beside(self):
        tracked = self.tree({
            "m/gfx/gfx.h": "/* gfx: pixels. */\n", "m/gfx/gfx.c": "/* gfx: more. */\n",
            "m/apps/x/app_x.c": "/* app_x: an app. */\n", "m/apps/x/other.h": "/* other: no. */\n",
            "t/suites.h": "/* The suites. */\n", "t/suites/suite_a.c": "/* suite_a: a. */\n",
        })
        self.assertEqual(self.describe("m/gfx", tracked), ("Pixels.", "m/gfx/gfx.h"))
        self.assertEqual(self.describe("m/apps/x", tracked), ("An app.", "m/apps/x/app_x.c"))
        self.assertEqual(self.describe("t/suites", tracked), ("The suites.", "t/suites.h"))

    def test_the_project_suffixes_name_what_is_looked_for(self):
        tracked = self.tree({"src/window/window.hpp": "// The window. More.\n", "src/window/window.h": "/* Not this. */\n"})
        self.assertEqual(self.describe("src/window", tracked, suffixes=(('.hpp', '.h'), ('.cpp',))),
                         ("The window. More.", "src/window/window.hpp"))

    def test_a_folder_readme_wins_over_its_sources(self):
        tracked = self.tree({"m/core/README.md": "# core\n\nThe platform base. More.\n",
                             "m/core/core.h": "/* core: not this. */\n"})
        self.assertEqual(self.describe("m/core", tracked), ("The platform base. More.", "m/core/README.md"))

    def test_optional_folder_description_is_empty_but_required_description_fails(self):
        tracked = self.tree({"m/odd/thing.c": "int x;\n", "m/mute/mute.h": "#pragma once\n"})
        for folder in ("m/odd", "m/mute"):
            with self.subTest(folder=folder):
                self.assertEqual(self.describe(folder, tracked, required=False), ("", folder))
                with self.assertRaisesRegex(model.Undescribed, re.escape(folder + "/")):
                    self.describe(folder, tracked)


class Modules(unittest.TestCase):
    def files(self, texts, headers=C[0], sources=C[1]):
        paths = sorted(texts)
        _, comp_of, _ = model.components_of(paths, ("m",), headers)
        files = model.source_files(texts, comp_of, set(paths), (), re.compile(r'^esp_'))
        model.pair_modules(files, comp_of, headers, sources)
        return {f['path']: f for f in files}

    def test_a_source_pairs_with_the_same_stem_header_it_includes(self):
        files = self.files({"m/a/x.c": '/* X. */\n#include "x.h"\n#include <esp_timer.h>\n', "m/a/x.h": "/* X API. */\n"})
        self.assertEqual(files["m/a/x.c"]["module"], "m/a/x")
        self.assertEqual(files["m/a/x.c"]["inc"], [["m/a/x.h", 1]])
        self.assertEqual(files["m/a/x.c"]["ext"], [["esp_timer.h", 1, None]])

    def test_cpp_sources_pair_by_the_suffixes_given(self):
        texts = {"m/a/frames.cpp": '#include "frames.hpp"\n', "m/a/frames.hpp": "// Frames. More.\n#pragma once\n"}
        self.assertEqual(self.files(texts, ('.hpp',), ('.cpp',))["m/a/frames.cpp"]["module"], "m/a/frames")

    def test_a_header_with_its_own_source_is_not_paired(self):
        files = self.files({"m/a/x.c": '/* X. */\n#include "x.h"\n', "m/b/x.c": '/* Other X. */\n#include "../a/x.h"\n',
                            "m/a/x.h": "/* X API. */\n"})
        self.assertNotIn("module", files["m/b/x.c"])

    def test_a_module_with_no_words_fails_naming_it(self):
        with self.assertRaisesRegex(model.Undescribed, "m/a/x.c has no description"):
            self.files({"m/a/x.c": '#include "x.h"\n', "m/a/x.h": "#pragma once\n"})


class Emitter(unittest.TestCase):
    def test_ids_nesting_quoting_and_counts(self):
        data = fixture(('apps/sand', 'render'))
        data['files'][0]['summary'] = "It's a \\ path.\nNext."
        data['files'][0]['ext'] = [['esp_timer.h', 2, 'launcher/components/vendor/esp_timer.h']]
        text = likec4.emit(data)
        self.assertIn("c_apps = component 'apps'", text)
        self.assertIn("c_sand = component 'apps/sand'", text)
        self.assertIn("m_icon = module 'icon'", text)
        self.assertIn("description 'It\\'s a \\\\ path. Next.'", text)
        self.assertIn("technology '1 file · 7 lines'", text)
        self.assertIn("technology 'icon.c'", text)
        self.assertIn('https://github.com/example/repo/blob/abc/launcher/components/vendor/esp_timer.h', text)
        self.assertIn('https://github.com/example/repo/blob/abc/launcher/main/apps/sand/icon.c', text)
        self.assertEqual(model.identifier('c_', 'raster/raster-motion'), 'c_raster_raster_motion')

    def test_source_links_have_titles_and_components_link_their_folder(self):
        text = likec4.emit(fixture(('gfx',)))
        self.assertIn("/tree/abc/launcher/main/gfx 'Source folder'", text)
        self.assertIn("/README.md 'Source: README.md'", text)

    def test_element_views_describe_their_scope(self):
        self.assertRegex(likec4.emit(fixture(('gfx',))), r"view c_gfx of [^{]+\{\n    title 'gfx'\n    description 'gfx'")

    def test_a_component_holds_its_own_modules_and_nested_components(self):
        lines = likec4.emit(fixture(('ui', 'ui/input', 'ui/input/deep'))).splitlines()
        self.assertIn('  view c_ui_input_deep of autana.firmware.c_ui.c_input.c_deep {', lines)
        ui, inp, deep = (next(i for i, l in enumerate(lines) if l.strip().startswith(f"c_{n} = component"))
                         for n in ('ui', 'input', 'deep'))
        indent = lambda i: len(lines[i]) - len(lines[i].lstrip())  # noqa: E731
        self.assertTrue(ui < inp < deep)
        self.assertEqual((indent(inp), indent(deep)), (indent(ui) + 2, indent(inp) + 2))
        self.assertIn("component 'ui/input'", lines[inp])
        # a component's size counts what is nested in it
        self.assertEqual(lines[ui + 2].strip(), "technology '3 files · 21 lines'")
        self.assertEqual(lines[deep + 2].strip(), "technology '1 file · 7 lines'")

    def test_an_empty_parent_component_still_nests_children(self):
        text = likec4.emit(fixture(('apps/sand', 'apps/sand/ui')))
        self.assertIn("c_apps = component 'apps'", text)
        self.assertIn('  view c_apps_sand_ui of autana.firmware.c_apps.c_sand.c_ui {', text.splitlines())

    def test_a_nested_component_view_folds_siblings_and_hides_what_another_path_covers(self):
        data = fixture(('ui', 'ui/input', 'ui/layout', 'ui/controls'))
        ui, inp, layout, controls = data['files']
        inp['inc'] = [[layout['path'], 1], [controls['path'], 1]]
        layout['inc'] = [[controls['path'], 1]]
        line = edge(likec4.emit(data), 'autana.firmware.c_ui.c_input.m_icon', 'autana.firmware.c_ui.c_controls.m_icon')
        self.assertIn('#indirect_c_ui ', line)
        self.assertNotIn('#indirect_c_ui_input ', line)

    def test_role_and_flag_tags_come_from_the_files(self):
        data = fixture(('game', 'game/ui', 'gfx'))
        data['files'][0]['app'] = True
        data['files'][-1]['iram'] = True
        text = likec4.emit(data)
        self.assertIn('#app', text)
        self.assertIn('#engine #iram', text)
        self.assertIn('tag iram { color red }', text)

    def test_without_roles_tags_or_release_view_none_are_written(self):
        data = fixture(('a',))
        for key in ('tags', 'roles', 'release_hidden'):
            data.pop(key)
        text = likec4.emit(data)
        for absent in ('#engine', 'tag engine', 'indirect_release', 'firmware_release'):
            self.assertNotIn(absent, text)
        self.assertIn('view firmware_all of autana.firmware', text)

    def test_tags_and_default_counts(self):
        data = fixture()
        a, b, c = data['files']
        a.update(variant='release', devcode=True)
        b.update(variant='dev', kind='test', gen=True)
        c.update(variant='diag', kind='tool')
        a['inc'] = [[b['path'], 2]]
        text = likec4.emit(data)
        self.assertIn('#devcode', text)
        self.assertIn('#dev #engine #generated #test', text)
        self.assertIn('#diag #engine #tool', text)
        line = edge(text, 'autana.firmware.c_a.m_icon', 'autana.firmware.c_b.m_icon')
        for tag in ('#dev', '#devcode', '#generated', '#test'):
            self.assertIn(tag, line)
        self.assertIn("includes '2'", line)
        self.assertIn("technology '0 files · 0 lines'", text)
        self.assertIn('exclude * where tag is #dev or tag is #diag', text)

    def test_module_pair_aggregation_and_component_tag_intersection(self):
        data = fixture(('a', 'b'))
        a, b = data['files']
        a.update(variant='dev', inc=[[b['path'], 2]])
        data['files'].append(dict(a, path=a['path'][:-1] + 'h', variant='diag'))
        line = edge(likec4.emit(data), 'autana.firmware.c_a.m_icon', 'autana.firmware.c_b.m_icon')
        self.assertIn("includes '4'", line)
        self.assertNotIn('#dev', line)
        self.assertNotIn('#diag', line)

    def test_reduction_is_per_view_and_keeps_all_arrows(self):
        data = fixture()
        a, b, c = data['files']
        a['inc'] = [[b['path'], 1], [c['path'], 1]]
        b.update(inc=[[c['path'], 1]], variant='dev')
        line = edge(likec4.emit(data), 'autana.firmware.c_a.m_icon', 'autana.firmware.c_c.m_icon')
        self.assertIn('#indirect ', line)
        self.assertNotIn('#indirect_release', line)
        self.assertNotIn('#indirect_c_a', line)
        self.assertEqual(likec4.redundant({('a', 'b'), ('b', 'c'), ('a', 'c')}), {('a', 'c')})

    def test_component_view_folds_outside_modules(self):
        data = fixture(('a', 'b'))
        a, b = data['files']
        other = dict(a, path='launcher/main/a/other.c', inc=[[b['path'], 1]])
        data['files'].append(other)
        a['inc'] = [[other['path'], 1], [b['path'], 1]]
        line = edge(likec4.emit(data), 'autana.firmware.c_a.m_icon', 'autana.firmware.c_b.m_icon')
        self.assertIn('#indirect_c_a', line)
        self.assertNotIn('#indirect_c_b', line)
        self.assertNotIn('#indirect ', line)

    def test_cycle_reduction_preserves_reachability(self):
        def reachable(edges, start):
            seen, todo = set(), [b for a, b in edges if a == start]
            while todo:
                node = todo.pop()
                if node not in seen:
                    seen.add(node)
                    todo.extend(b for a, b in edges if a == node)
            return seen
        cycle = {('a', 'b'), ('b', 'a'), ('a', 'c'), ('b', 'c')}
        cut = likec4.redundant(cycle)
        self.assertEqual(len(cut & {('a', 'c'), ('b', 'c')}), 1)
        self.assertTrue(all('c' in reachable(cycle - cut, n) for n in ('a', 'b')))
        rng = random.Random(11)
        edges = {(a, b) for a in range(16) for b in range(16) if a != b and rng.random() < .23}
        kept = edges - likec4.redundant(edges)
        for node in range(16):
            self.assertEqual(reachable(kept, node), reachable(edges, node))
        self.assertEqual(likec4.redundant(list(reversed(sorted(edges)))), edges - kept)

    def test_component_tags_ignore_hidden_modules(self):
        for kind, gen in (('test', False), ('tool', False), ('src', True)):
            for variant in ('release', None):
                data = fixture(('a',))
                a = data['files'][0]
                a['variant'] = 'dev'
                data['files'].append(dict(a, path='launcher/main/a/hidden.c', kind=kind,
                                          gen=gen, variant=variant))
                header = likec4.emit(data).split("c_a = component 'a' {", 1)[1].split('m_', 1)[0]
                self.assertIn('#dev', header)
        data['files'] = [dict(a, kind='test')]
        header = likec4.emit(data).split("c_a = component 'a' {", 1)[1].split('m_', 1)[0]
        self.assertIn('#dev #engine #test', header)

    def test_the_drill_view_folds_apps_to_the_parent_component(self):
        data = fixture(('apps/sand', 'apps/other', 'b', 'c'))
        sand, other, b, c = data['files']
        sand['inc'] = [[b['path'], 1], [c['path'], 1]]
        b['inc'] = [[other['path'], 1]]
        other['inc'] = [[c['path'], 1]]
        self.assertNotIn('#indirect ', edge(likec4.emit(data), 'autana.firmware.c_apps.c_sand.m_icon',
                                            'autana.firmware.c_c.m_icon'))

    def test_component_view_keeps_sibling_components_distinct(self):
        data = fixture(('apps/a', 'apps/b', 'apps/c'))
        a, b, c = data['files']
        other = dict(a, path='launcher/main/apps/a/other.c', inc=[[b['path'], 1]])
        data['files'].append(other)
        a['inc'] = [[other['path'], 1], [c['path'], 1]]
        self.assertNotIn('#indirect_c_apps_a', edge(likec4.emit(data), 'autana.firmware.c_apps.c_a.m_icon',
                                                     'autana.firmware.c_apps.c_c.m_icon'))

    def test_component_tag_namespace_and_default_hidden(self):
        with mock.patch.object(likec4, 'DEFAULT_HIDDEN', {'custom'}):
            text = likec4.emit(fixture(('release',)))
        self.assertIn('tag indirect_c_release', text)
        self.assertIn('tag custom', text)
        self.assertIn('exclude * where tag is #custom', text)
        self.assertEqual(text.count('  tag indirect_release\n'), 1)

    def test_the_model_folder_holds_the_system_model_and_its_config(self):
        with tempfile.TemporaryDirectory() as temp:
            path = likec4.write(temp, fixture(), 'model {}\n')
            self.assertEqual(path.name, 'autana.c4')
            config = json.loads((pathlib.Path(temp) / 'likec4.config.json').read_text())
        self.assertEqual(config, dict(name='Fixture architecture', title='Fixture architecture',
                                      styles=dict(defaults=dict(view=dict(routing='ortho')))))
        self.assertIn('sizing fixed', likec4.emit(fixture()))

    def test_a_deployment_adds_its_kinds_block_and_views(self):
        class Deployment:
            kinds = ('computer',)

            def emit(self, out, base, containers):
                out.extend(['', 'deployment {', "  pc = computer 'PC' {", f'    image = instanceOf {base}', '  }', '}'])
                return ['  deployment view pc {', '    include pc', '  }']
        text = likec4.emit(fixture(), Deployment())
        self.assertIn("deploymentNode computer { notation 'Computer' }", text)
        self.assertIn('image = instanceOf autana.firmware', text)
        self.assertTrue(text.rstrip().endswith('  deployment view pc {\n    include pc\n  }\n}'))
        validate(self, text)

    def test_installed_validator_accepts_fixture_tree_and_rejects_broken_model(self):
        data = fixture(('apps/sand', 'render'))
        data['files'][0]['summary'] = "It's a \\ path: https://example.com."
        validate(self, likec4.emit(data))
        with self.assertRaises(subprocess.CalledProcessError):
            validate(self, 'model { broken = unknown }')


def banded():
    data = fixture(('apps', 'shell', 'core', 'gfx', 'main'))
    place = dict(apps=('top', 0), shell=('top', 1), gfx=('mid', 0), core=('base', 0))
    for c in data['components']:
        if c['id'] in place:
            c['band'], c['row'] = place[c['id']]
    data['bands'] = [dict(id=b, desc=b + ' rows', src='docs/firmware/Layers.md') for b in ('top', 'mid', 'base')]
    apps, shell, core, gfx, main = data['files']
    apps['inc'] = [[shell['path'], 1], [gfx['path'], 1]]
    shell['inc'] = [[gfx['path'], 1]]
    gfx['inc'] = [[core['path'], 1]]
    main['inc'] = [[shell['path'], 1]]
    return data


class Bands(unittest.TestCase):
    def test_a_banded_folder_nests_in_its_band_and_main_stays_beside_the_bands(self):
        text = likec4.emit(banded())
        lines = text.splitlines()
        self.assertIn("description 'top rows'", text)
        self.assertIn('  view b_top of autana.firmware.b_top {', lines)
        self.assertIn('  view c_apps of autana.firmware.b_top.c_apps {', lines)
        self.assertIn("      c_main = component 'main' {", lines)
        self.assertLess(next(i for i, l in enumerate(lines) if "b_top = band 'top'" in l),
                        next(i for i, l in enumerate(lines) if "c_apps = component" in l))

    def test_a_band_holds_its_folders_row_by_row(self):
        lines = likec4.emit(banded()).splitlines()
        order = [l.strip().split()[0] for l in lines if ' = component ' in l and l.startswith('        c_')]
        self.assertEqual(order, ['c_apps', 'c_shell', 'c_gfx', 'c_core'])

    def test_the_drill_view_folds_to_bands_and_hides_an_arrow_another_path_covers(self):
        data = banded()
        data['files'][0]['inc'].append([data['files'][2]['path'], 1])
        text = likec4.emit(data)
        # apps -> core joins top to base; top -> mid -> base already does, so that band arrow is hidden
        self.assertIn('#indirect ', edge(text, 'autana.firmware.b_top.c_apps.m_icon', 'autana.firmware.b_base.c_core.m_icon'))
        self.assertNotIn('#indirect ', edge(text, 'autana.firmware.b_top.c_shell.m_icon', 'autana.firmware.b_mid.c_gfx.m_icon'))
        self.assertNotIn('#indirect ', edge(text, 'autana.firmware.b_mid.c_gfx.m_icon', 'autana.firmware.b_base.c_core.m_icon'))
        self.assertNotIn('#indirect ', edge(text, 'autana.firmware.c_main.m_icon', 'autana.firmware.b_top.c_shell.m_icon'))

    def test_a_band_view_has_its_own_hiding_tag(self):
        text = likec4.emit(banded())
        self.assertIn('tag indirect_b_top', text)
        self.assertIn('or tag is #indirect_b_top\n', text)
        self.assertIn('or tag is #indirect_b_base\n', text)

    def test_the_installed_validator_accepts_the_banded_model(self):
        validate(self, likec4.emit(banded()))


class RelationshipTitles(unittest.TestCase):
    def titled(self):
        data = banded()
        apps = data['files'][0]
        for name in ('a.h', 'b.h', 'c.h'):
            data['files'].append(dict(data['files'][3], path='launcher/main/gfx/' + name, inc=[]))
            apps['inc'].append([data['files'][-1]['path'], 1])
        data['links'] = [dict({'from': 'firmware', 'to': 'firmware'}, components=['collections']),
                         dict({'from': 'firmware', 'to': 'firmware'}, tag='dev', components=['one', 'two', 'three', 'four'])]
        return data

    def relationships(self, text, kind):
        return [l for l in text.splitlines() if f'-[{kind}]->' in l]

    def title(self, line):
        return re.search(r"-> \S+ '([^']+)' \{", line).group(1)

    def test_links_are_titled_and_includes_are_left_untitled(self):
        text = likec4.emit(self.titled())
        for line in self.relationships(text, 'links'):
            with self.subTest(line=line):
                self.assertRegex(line, r"-\[links\]-> \S+ '[^']+' \{")
        for line in self.relationships(text, 'includes'):
            with self.subTest(line=line):
                self.assertRegex(line, r"-\[includes\]-> \S+ \{")
        self.assertEqual(likec4.shortlist(['a.h', 'b.h', 'c.h', 'd.h']), 'a.h, b.h, +2')

    def test_a_links_tag_marks_it(self):
        lines = self.relationships(likec4.emit(self.titled()), 'links')
        self.assertNotIn('#', lines[0])
        self.assertIn('{ #dev metadata', lines[1])

    def test_the_full_header_list_stays_in_metadata(self):
        data = banded()
        apps, shell, core, gfx, main = data['files']
        apps['inc'] = [[gfx['path'], 3], [shell['path'], 1]]
        line = edge(likec4.emit(data), 'autana.firmware.b_top.c_apps.m_icon', 'autana.firmware.b_mid.c_gfx.m_icon')
        self.assertRegex(line, r"-\[includes\]-> \S+ \{")
        self.assertIn("headers 'icon.c: 3'", line)

    def test_a_links_title_lists_the_required_components_then_how_many_more(self):
        titles = [self.title(l) for l in self.relationships(likec4.emit(self.titled()), 'links')]
        self.assertEqual(titles, ['requires collections', 'requires one, two, +2'])


class Packages(unittest.TestCase):
    def test_counts_are_pluralised(self):
        self.assertEqual(likec4.size_text([dict(lines=1)]), '1 file · 1 line')
        self.assertEqual(likec4.size_text([dict(lines=2), dict(lines=0)]), '2 files · 2 lines')

    def test_package_container_owns_children_and_its_view(self):
        data = fixture(('math', 'math/linear'))
        data['packages'] = [dict(id='math', folder='launcher/main/math', desc='Numbers.', src='README.md')]
        for c in data['components']:
            c['package'] = 'math'
        text = likec4.emit(data)
        self.assertNotIn("c_math = component", text)
        self.assertIn('view c_math of autana.math {', text)
        self.assertIn('view c_math_linear of autana.math.c_linear {', text)
        self.assertIn('tag is #indirect_c_math', text)

    def test_the_model_nests_the_packages_in_one_box_with_its_own_view(self):
        data = fixture(('apps',))
        data['containers']['nodes'].append(dict(id='editor', name='Editor', type='container', desc='Editor', src='README.md', tech='C'))
        data['packages'] = [dict(id=n, folder='launcher/packages/' + n, desc=n, src='README.md') for n in ('math', 'name_hash')]
        data['packages_box'] = dict(id='packages', name='Packages', desc='Libraries.', src='launcher/CMakeLists.txt', members=['math', 'name_hash'])
        text = likec4.emit(data)
        lines = text.splitlines()
        self.assertIn("    packages = container 'Packages' {", lines)
        self.assertIn("      math = container 'math' {", lines)
        self.assertIn("      name_hash = container 'name_hash' {", lines)
        self.assertIn("    editor = container 'Editor' {", lines)
        self.assertIn('  view packages of autana.packages {', lines)
        self.assertIn('tag indirect_p_packages', text)

    def test_the_installed_validator_accepts_the_nested_packages(self):
        data = fixture(('apps',))
        data['packages'] = [dict(id='math', folder='launcher/packages/math', desc='math', src='README.md')]
        data['packages_box'] = dict(id='packages', name='Packages', desc='Libraries.', src='README.md', members=['math'])
        data['links'] = [dict({'from': 'firmware', 'to': 'math'}, components=['math'])]
        validate(self, likec4.emit(data))


class Command(unittest.TestCase):
    """build.py on a C++ project file: a fixture checkout read through git."""
    PROJECT = dict(
        people=[["user", "User", "Opens captures."]],
        systems=[["engine", "Engine", "The engine it reads.", "third_party/engine"]],
        containers=[["app", "Viewer", "C++20", "src/main.cpp"]],
        relations=[["user", "app", "opens"], ["app", "engine", "reads"]],
        drill="app", scan=["src", "tests"], include_roots=["src", "tests"],
        headers=[".hpp", ".h"], sources=[".cpp"], vendor_includes="^(SDL3/|imgui)")
    FILES = {
        "README.md": "# Viewer\n\nShows what the engine drew.\n",
        "src/main.cpp": '// Opens the window. More.\n#include "window/window.hpp"\n#include <SDL3/SDL.h>\n',
        "src/window/window.hpp": "// The window: one per capture.\n#pragma once\n",
        "src/window/window.cpp": '#include "window/window.hpp"\n',
        "tests/window_test.cpp": '// Window tests.\n#include "window/window.hpp"\n',
        "third_party/engine/README.md": "# Engine\n",
    }

    def checkout(self, project=None, files=None):
        tree = write_tree(self, dict(self.FILES, **(files or {})))
        git(tree.root, "init", "-q", "-b", "main")
        git(tree.root, "remote", "add", "origin", "https://github.com/example/viewer.git")
        git(tree.root, "add", ".")
        git(tree.root, "commit", "-q", "-m", "fixture")
        path = tree.root / "project.json"
        path.write_text(json.dumps(project or self.PROJECT), encoding="utf-8")
        return tree, path

    def test_a_cpp_project_is_drawn_from_its_project_file(self):
        tree, project = self.checkout()
        out = tree.root / "out"
        with mock.patch("sys.stdout"):
            build.main(["--project", str(project), "--root", str(tree.root), "-o", str(out)])
        text = (out / "viewer.c4").read_text(encoding="utf-8")
        self.assertIn("viewer = system 'Viewer'", text)
        self.assertIn("app = container 'Viewer'", text)
        self.assertIn("engine = external 'Engine'", text)
        self.assertIn("m_window = module 'window'", text)
        self.assertIn("technology 'window.cpp, window.hpp'", text)
        self.assertIn("vendor_includes 'SDL3/SDL.h: 1'", text)
        self.assertIn("c_tests = component 'tests' {\n        #test", text)
        self.assertIn("viewer.app.c_src.m_main -[includes]-> viewer.app.c_window.m_window", text)
        self.assertIn("https://github.com/example/viewer/blob/", text)
        self.assertIn("view app of viewer.app", text)
        self.assertEqual(json.loads((out / "likec4.config.json").read_text())["title"], "Viewer architecture")
        validate(self, text)

    def test_a_project_file_with_a_missing_or_unknown_key_fails_naming_it(self):
        _, project = self.checkout(project={k: v for k, v in self.PROJECT.items() if k != "drill"} | {"extra": 1})
        with self.assertRaisesRegex(SystemExit, r"missing \['drill'\], unknown \['extra'\]"):
            build.main(["--project", str(project), "-o", "unused"])

    def test_a_system_the_tree_no_longer_holds_fails_naming_its_folder(self):
        project = dict(self.PROJECT, systems=[["engine", "Engine", "Gone.", "third_party/gone"]])
        tree, path = self.checkout(project=project)
        with self.assertRaisesRegex(SystemExit, "third_party/gone/"):
            build.main(["--project", str(path), "--root", str(tree.root), "-o", str(tree.root / "out")])


class Site(unittest.TestCase):
    def test_missing_install_names_command(self):
        with tempfile.TemporaryDirectory() as temp, mock.patch.object(site, 'PACKAGE', pathlib.Path(temp)):
            with self.assertRaisesRegex(SystemExit, re.escape('npm ci --prefix ' + str(pathlib.Path(temp).parent))):
                site.validate(temp, {'name': 'fixture'})

    def test_validation_precedes_build_and_failure_stops_it(self):
        with mock.patch.object(pathlib.Path, 'read_text', return_value='{"name": "fixture", "title": "Fixture"}'), \
             mock.patch.object(site, 'validate', side_effect=subprocess.CalledProcessError(1, ['node'])) as check, \
             mock.patch.object(site.subprocess, 'run') as run:
            with self.assertRaises(subprocess.CalledProcessError):
                site.main(['missing', '-o', 'output'])
            check.assert_called_once()
            run.assert_not_called()

    def test_missing_config_fails_before_running(self):
        with tempfile.TemporaryDirectory() as temp, mock.patch.object(site.subprocess, 'run') as run:
            with self.assertRaises(FileNotFoundError):
                site.main([temp, '-o', str(pathlib.Path(temp) / 'out')])
            run.assert_not_called()

    def test_build_arguments_use_project_config(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / 'likec4.config.json').write_text(json.dumps(dict(name='fixture', title='Config title')), encoding='utf-8')
            with mock.patch.object(site.subprocess, 'run') as run:
                site.main([temp, '-o', str(root / 'site'), '--base', '/repo/architecture/'])
            self.assertEqual(run.call_count, 2)
            check, build_call = [call.args[0] for call in run.call_args_list]
            self.assertEqual(check[0], 'node')
            self.assertEqual(check[2:], ['validate', temp, '--project', 'fixture'])
            self.assertEqual(build_call[2:], ['build', temp, '-o', str(root / 'site'), '--use-hash-history',
                                             '--title', 'Config title', '--project', 'fixture', '--base', '/repo/architecture/'])
            self.assertTrue(all(call.kwargs['check'] for call in run.call_args_list))


if __name__ == '__main__':
    unittest.main()
