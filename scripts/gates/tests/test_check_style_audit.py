"""Regression tests for scripts/gates/check_style_audit.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest
import gate_tree

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import check_style_audit  # noqa: E402


class StyleAuditTest(unittest.TestCase):
    def rule_hits(self, root, rule_id, file_filter=None):
        findings, _ = check_style_audit.run_audit(root, rule_filter=rule_id, file_filter=file_filter)
        return findings

    def test_undef_requires_analysis_scan_ifdef(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/defines.c",
                            "#ifdef ANALYSIS_SCAN\n#undef A\n#ifdef INNER\n#undef B\n#endif\n"
                            "#else\n#undef C\n#endif\n#undef D\n")
            gate_tree.commit(root, "launcher")
            hits = self.rule_hits(root, "UNDEF-PLACEMENT")
            self.assertEqual([hit.line for hit in hits], [7, 9])

    def test_a_bd_issue_id_in_a_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/gfx.c",
                            "/* gfx - fixture header. */\n\n"
                            "void\ngfx_init(void) {\n"
                            "    /* see bd autana-zq7 for the follow-up */\n}\n")
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "TRACKER-REF")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].severity, "ERROR")

    def test_a_bare_digit_led_tracker_id_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "See autana-9qz for the follow-up.\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "TRACKER-REF")
        self.assertEqual(len(findings), 1)

    def test_a_full_commit_hash_in_a_doc_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md",
                            "Pinned to commit 8275e0af7c16aa40c54ea2b90b7af83b1fe4eb4c.\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "TRACKER-REF")
        self.assertEqual(len(findings), 1)

    def test_real_compound_words_and_a_short_build_id_are_not_flagged(self):
        # autana-cli, autana-screenshot, autana-monitor and autana-device
        # are all real, tracked, letter-only compounds in this repo; none
        # of them may start looking like a tracker id.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md",
                            "Run `autana screenshot` (autana-screenshot), `autana monitor` "
                            "(autana-monitor), autana-cli, or check autana-device; "
                            "build a195574e7177-diag.\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "TRACKER-REF")
        self.assertEqual(findings, [])

    def test_a_date_in_a_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/gfx.c",
                            "/* gfx - fixture header. */\n\n"
                            "void\ngfx_init(void) {\n"
                            "    /* measured on device, 2026-09-13 */\n}\n")
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "CALENDAR-DATE")
        self.assertEqual(len(findings), 1)

    def test_a_date_in_a_dated_log_txt_file_is_not_scanned(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/doc_review_ledger.txt", "docs/Guide.md\t2026-09-17\tchecked\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "CALENDAR-DATE")
        self.assertEqual(findings, [])

    def test_living_document_phrase_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "This is a living document.\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "LIVING-DOC")
        self.assertEqual(len(findings), 1)

    def gfx_fixture(self, root):
        gate_tree.write(root, "launcher/main/gfx/gfx.h", "#pragma once\n")

    def test_a_relative_include_of_a_layer_header_is_flagged_and_fixed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.gfx_fixture(root)
            target = root / "launcher/main/apps/sand/app_sand.c"
            gate_tree.write(root, "launcher/main/apps/sand/app_sand.c", '#include "../../gfx/gfx.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-LAYER")
            self.assertEqual(len(findings), 1)
            self.assertTrue(findings[0].fixable)
            check_style_audit.run_fix(root, findings)
            self.assertEqual(target.read_text(encoding="utf-8"), '#include "gfx/gfx.h"\n')

    def test_an_apps_own_local_header_sharing_a_layer_basename_is_not_rewritten(self):
        # The compiler resolves a quoted include next to the including file
        # FIRST; apps/foo/fixed.h is found before launcher/main/fixed.h (or
        # any layer/fixed.h) is even tried, so this is never a layer-header
        # reference at all, however many other folders happen to have a
        # file with the same basename.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/util/scalar/fixed.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/apps/foo/fixed.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/apps/foo/app_foo.c", '#include "fixed.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-LAYER")
        self.assertEqual(findings, [])

    def test_a_bare_include_that_resolves_nowhere_is_not_flagged(self):
        # Two layers each have a same-named header and neither sits at the
        # include root itself; the real compiler would not find this
        # include either, so it is a compile error, not this rule's concern.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/shared.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/ui/shared.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/apps/sand/app_sand.c", '#include "shared.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-LAYER")
        self.assertEqual(findings, [])

    def test_a_relative_reach_into_board_is_flagged(self):
        # LAYER_DIRS used to be hand-typed and missed board/ entirely, so a
        # relative reach into it went unflagged.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/board/board.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/apps/sand/app_sand.c", '#include "../../board/board.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-LAYER")
        self.assertEqual(len(findings), 1)
        self.assertIn("board/board.h", findings[0].message)

    def test_an_apps_internal_relative_include_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/apps/sand/material.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/apps/sand/ui/brush_screen.c",
                            '#include "../material.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-LAYER")
        self.assertEqual(findings, [])

    def layer_tree(self, root):
        # The real layer folders (LAYER_TIER minus the virtual "apps" entry),
        # minimally populated, so rule_include_direction's own
        # LAYER_TIER-vs-tree check passes.
        for layer in check_style_audit.LAYER_DIRS:
            gate_tree.write(root, f"launcher/main/{layer}/.keep", "")

    def test_a_same_tier_cross_folder_include_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/render/r3d_project.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.c", '#include "render/r3d_project.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(len(findings), 1)
        self.assertIn("tier", findings[0].message)

    def test_include_direction_judges_the_resolved_file_not_the_spelling(self):
        # A relative include from gfx/ into ui/ must be caught before
        # INCLUDE-LAYER's own --fix ever runs, since it reaches sideways
        # within the same tier either way.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/ui/ui.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.c", '#include "../ui/ui.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(len(findings), 1)

    def test_a_lower_layer_including_an_app_header_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/apps/foo/foo_api.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.c", '#include "apps/foo/foo_api.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(len(findings), 1)
        self.assertIn("apps/", findings[0].message)

    def test_apps_own_sources_are_never_a_source_for_include_direction(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/util/runtime/tune.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/apps/foo/app_foo.c", '#include "util/runtime/tune.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(findings, [])

    def test_including_a_strictly_lower_tier_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/util/runtime/tune.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.c", '#include "util/runtime/tune.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(findings, [])

    def test_a_same_folder_include_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/gfx/gfx_dirty.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.c", '#include "gfx/gfx_dirty.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(findings, [])

    def test_util_may_not_include_display(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/display/display.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/util/runtime/device_state.c", '#include "display/display.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(len(findings), 1)

    def test_display_and_input_may_not_include_each_other_and_gfx_not_display(self):
        # display/ is handed gravity by the shell; neither it nor input/ reads the other.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/display/display.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/input/input.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/display/display.c", '#include "gfx/gfx.h"\n')
            gate_tree.write(root, "launcher/main/display/display_device.c", '#include "input/input.h"\n')
            gate_tree.write(root, "launcher/main/input/input_device.c", '#include "display/display.h"\n')
            gate_tree.write(root, "launcher/main/gfx/gfx.c", '#include "display/display.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(sorted(f.path for f in findings),
                         ["launcher/main/display/display_device.c", "launcher/main/gfx/gfx.c",
                          "launcher/main/input/input_device.c"])

    def test_boot_and_selftest_may_not_include_each_other(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/boot/boot_anim.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/selftest/post.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/boot/boot_anim.c", '#include "selftest/post.h"\n')
            gate_tree.write(root, "launcher/main/selftest/post.c", '#include "boot/boot_anim.h"\n#include "gfx/gfx.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(sorted((f.path, f.line) for f in findings),
                         [("launcher/main/boot/boot_anim.c", 1), ("launcher/main/selftest/post.c", 1)])

    PURE_UTIL = ("math", "motion", "encode", "scalar", "build")

    def util_tree(self, root):
        self.layer_tree(root)
        for sub in ("runtime",) + self.PURE_UTIL:
            gate_tree.write(root, f"launcher/main/util/{sub}/{sub}.h", "#pragma once\n")

    def test_no_pure_util_folder_may_include_its_runtime_services(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.util_tree(root)
            for sub in self.PURE_UTIL:
                gate_tree.write(root, f"launcher/main/util/{sub}/{sub}.c", '#include "util/runtime/runtime.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(sorted(f.path for f in findings),
                         sorted(f"launcher/main/util/{sub}/{sub}.c" for sub in self.PURE_UTIL))

    def test_runtime_may_include_every_pure_util_folder_and_scalar_sits_under_math(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.util_tree(root)
            gate_tree.write(root, "launcher/main/util/runtime/job.c",
                            "".join(f'#include "util/{sub}/{sub}.h"\n' for sub in self.PURE_UTIL))
            gate_tree.write(root, "launcher/main/util/math/vec3x.h", '#include "util/scalar/scalar.h"\n')
            gate_tree.write(root, "launcher/main/util/scalar/fixed.h", '#include "util/math/math.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual([f.path for f in findings], ["launcher/main/util/scalar/fixed.h"])

    def test_a_file_loose_in_a_split_folder_fails_loudly_but_its_description_header_does_not(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.util_tree(root)
            gate_tree.write(root, "launcher/main/util/util.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.c", "int x;\n")
            gate_tree.commit(root, "launcher")
            self.assertEqual(self.rule_hits(root, "INCLUDE-DIRECTION"), [])
            gate_tree.write(root, "launcher/main/util/perf_region.c", "int x;\n")
            gate_tree.commit(root, "launcher")
            with self.assertRaisesRegex(ValueError, "util/perf_region.c"):
                self.rule_hits(root, "INCLUDE-DIRECTION")

    def test_a_subfolder_of_a_split_folder_this_table_does_not_know_about_fails_loudly(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/gfx/gfx.c", "int x;\n")
            gate_tree.write(root, "launcher/main/util/newkind/thing.h", "#pragma once\n")
            gate_tree.commit(root, "launcher")
            with self.assertRaisesRegex(ValueError, "util/newkind"):
                self.rule_hits(root, "INCLUDE-DIRECTION")

    def test_a_folder_this_table_does_not_know_about_fails_loudly(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/gfx/gfx.c", "int x;\n")
            gate_tree.write(root, "launcher/main/newlayer/thing.h", "#pragma once\n")
            gate_tree.commit(root, "launcher")
            with self.assertRaises(ValueError):
                self.rule_hits(root, "INCLUDE-DIRECTION")

    def test_a_windows_personal_path_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "scripts/device/device.py",
                            'IDF_PYTHON = r"C:\\Users\\jdoe\\.espressif\\python_env\\python.exe"\n')
            gate_tree.commit(root, "scripts")
            findings = self.rule_hits(root, "PERSONAL-PATH")
        self.assertEqual(len(findings), 1)

    def test_a_drive_rooted_esp_idf_checkout_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/tools/build.sh",
                            "DEFAULT_EXPORT='" + "\\".join(["C:", "Espressif", "esp-idf-v5.5", "export.bat"]) + "'\n")
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "PERSONAL-PATH")
        self.assertEqual(len(findings), 1)

    def test_any_drive_path_to_an_esp_idf_checkout_is_flagged(self):
        # The layouts an installer actually produces, not only one machine's.
        layouts = ["\\".join(["C:", "Espressif", "v5.5.5", "esp-idf", "export.bat"]),
                   "\\".join(["D:", "esp", "esp-idf", "export.bat"]),
                   "/".join(["C:", "Espressif", "v5.5.5", "esp-idf"])]
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/tools/build.sh",
                            "".join(f"EXPORT_{i}='{layout}'\n" for i, layout in enumerate(layouts)))
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "PERSONAL-PATH")
        self.assertEqual(len(findings), len(layouts))

    def test_portable_esp_idf_references_are_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/tools/build.sh",
                            'EXPORT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"\n'
                            "# https://github.com/espressif/esp-idf\n")
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "PERSONAL-PATH")
        self.assertEqual(findings, [])

    def test_a_portable_home_relative_path_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "scripts/device/device.py",
                            'GLOB = "~/.espressif/python_env/idf*_env/Scripts/python.exe"\n')
            gate_tree.commit(root, "scripts")
            findings = self.rule_hits(root, "PERSONAL-PATH")
        self.assertEqual(findings, [])

    def test_a_tool_named_after_an_app_model_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/apps/lab/assets/Otter.blend", "model")
            gate_tree.write(root, "launcher/main/apps/lab/tools/export_otter_glb.py", "pass\n")
            gate_tree.write(root, "scripts/otter_bake.sh", "true\n")
            gate_tree.commit(root, "launcher", "scripts")
            findings = self.rule_hits(root, "MODEL-NAMED-TOOL")
        self.assertEqual(sorted(f.path for f in findings),
                         ["launcher/main/apps/lab/tools/export_otter_glb.py", "scripts/otter_bake.sh"])

    def test_app_runtime_code_and_generic_tools_may_name_or_skip_models(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/apps/lab/assets/otter.glb", "model")
            gate_tree.write(root, "launcher/main/apps/lab/scene_otter.c", "int x;\n")
            gate_tree.write(root, "launcher/tools/gltf/blend_skin_to_glb.py", "pass\n")
            gate_tree.write(root, "docs/images/otter.png", "image")
            gate_tree.commit(root, "launcher", "docs")
            findings = self.rule_hits(root, "MODEL-NAMED-TOOL")
        self.assertEqual(findings, [])

    def test_a_stray_html_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "text\n<!-- TODO: remove this -->\nmore text\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "STRAY-HTML-COMMENT")
        self.assertEqual(len(findings), 1)

    def test_a_multi_line_html_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "text\n<!-- a stray aside\nspanning two lines -->\nmore\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "STRAY-HTML-COMMENT")
        self.assertEqual(len(findings), 1)

    def test_a_known_escape_marker_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md",
                            "C6 is historical. <!-- doc-vocabulary: ignore -->\n"
                            "<!-- BEGIN GENERATED -->\ntable\n<!-- END GENERATED -->\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "STRAY-HTML-COMMENT")
        self.assertEqual(findings, [])

    def test_a_named_generated_block_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md",
                            "<!-- generated: scores sha256=" + "0" * 64 + " -->\n| 2 |\n"
                            "<!-- /generated: scores -->\n<!-- generated: notes -->\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "STRAY-HTML-COMMENT")
        self.assertEqual(findings, [])

    def test_a_bullet_right_after_unindented_prose_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md",
                            "This sentence continues onto the next line where it\n"
                            "- 3 happens to start with a dash and a space\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(len(findings), 1)

    def test_a_bullet_after_an_indented_list_continuation_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md",
                            "- **First item.** Some wrapped continuation text\n"
                            "  that is indented under the item above.\n"
                            "- Second item.\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(findings, [])

    def test_a_lazily_continued_list_item_is_not_flagged(self):
        # CommonMark's lazy continuation lets a list item's paragraph carry
        # on with no indentation at all; the paragraph this "- " sits in
        # started with its own list marker, so it is a sibling item, not an
        # accident.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md",
                            "- First item text\n"
                            "continues here with no indentation at all\n"
                            "- Second item\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(findings, [])

    def test_a_list_right_after_a_closing_fence_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "```sh\nexample\n```\n- a real list item\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(findings, [])

    def test_a_real_list_introduced_by_a_colon_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "The rules are:\n- one\n- two\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(findings, [])

    def test_consecutive_blank_lines_are_flagged_and_fixed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = root / "docs/Guide.md"
            gate_tree.write(root, "docs/Guide.md", "one\n\n\ntwo\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "BLANK-LINES")
            self.assertEqual(len(findings), 1)
            check_style_audit.run_fix(root, findings)
            self.assertEqual(target.read_text(encoding="utf-8"), "one\n\ntwo\n")

    def test_a_generated_tables_own_blank_lines_are_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Table.md",
                            "<!-- BEGIN GENERATED -->\na\n\n\nb\n<!-- END GENERATED -->\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "BLANK-LINES")
        self.assertEqual(findings, [])

    def test_trailing_blank_lines_are_flagged_and_fixed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = root / "docs/Guide.md"
            gate_tree.write(root, "docs/Guide.md", "one\ntwo\n\n\n")
            gate_tree.commit(root, "docs")
            findings = self.rule_hits(root, "TRAILING-BLANK-LINES")
            self.assertEqual(len(findings), 1)
            check_style_audit.run_fix(root, findings)
            self.assertEqual(target.read_text(encoding="utf-8"), "one\ntwo\n")

    def test_a_drawn_rule_comment_is_flagged_and_fixed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = root / "launcher/main/gfx/gfx.c"
            gate_tree.write(root, "launcher/main/gfx/gfx.c",
                            "/* gfx - fixture header. */\n\n"
                            "/* ==== Setup ==== */\nvoid\ngfx_init(void) {}\n")
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "DRAWN-COMMENT-RULE")
            self.assertEqual(len(findings), 1)
            check_style_audit.run_fix(root, findings)
            self.assertIn("/* Setup */", target.read_text(encoding="utf-8"))
            self.assertNotIn("====", target.read_text(encoding="utf-8"))

    def test_a_title_case_comment_inside_a_function_body_is_a_warning(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/gfx.c",
                            "/* gfx - fixture header. */\n\n"
                            "void\nfoo(void) {\n    int a = 1;\n    /* Setup */\n    a = 2;\n}\n")
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "HEADING-COMMENT")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].severity, "WARN")

    def test_the_same_text_as_a_file_level_divider_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/gfx.c",
                            "/* Setup */\n\nstatic int\nfoo(void) {\n    return 1;\n}\n")
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "HEADING-COMMENT")
        self.assertEqual(findings, [])

    def test_the_same_text_inside_an_array_initializer_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/gfx.c",
                            "static const int table[] = {\n    /* Values */\n    1, 2, 3,\n};\n")
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "HEADING-COMMENT")
        self.assertEqual(findings, [])


class MainTest(unittest.TestCase):
    def run_main(self, root, argv):
        import os
        cwd = os.getcwd()
        try:
            os.chdir(root)
            return check_style_audit.main(argv)
        finally:
            os.chdir(cwd)

    def test_a_clean_tree_exits_zero(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "Nothing to see here.\n")
            gate_tree.commit(root, "docs")
            code = self.run_main(root, [])
        self.assertEqual(code, 0)

    def test_an_error_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "This is a living document.\n")
            gate_tree.commit(root, "docs")
            code = self.run_main(root, [])
        self.assertEqual(code, 1)

    def test_an_unknown_layer_folder_exits_two_instead_of_crashing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/gfx.c", "int x;\n")
            gate_tree.write(root, "launcher/main/newlayer/thing.h", "#pragma once\n")
            gate_tree.commit(root, "launcher")
            code = self.run_main(root, ["--rule", "INCLUDE-DIRECTION"])
        self.assertEqual(code, 2)

    def test_a_warning_alone_passes_without_strict_and_fails_with_it(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/gfx.c",
                            "/* gfx - fixture header. */\n\n"
                            "void\nfoo(void) {\n    /* Setup */\n    bar();\n}\n")
            gate_tree.commit(root, "launcher")
            plain = self.run_main(root, [])
            strict = self.run_main(root, ["--strict"])
        self.assertEqual(plain, 0)
        self.assertEqual(strict, 1)

    def test_fix_rewrites_the_file_and_the_rerun_is_clean(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = root / "docs/Guide.md"
            gate_tree.write(root, "docs/Guide.md", "one\n\n\ntwo\n")
            gate_tree.commit(root, "docs")
            code = self.run_main(root, ["--fix"])
            self.assertEqual(code, 0)
            self.assertEqual(target.read_text(encoding="utf-8"), "one\n\ntwo\n")

    def test_a_file_filter_matching_nothing_exits_two(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "text\n")
            gate_tree.commit(root, "docs")
            code = self.run_main(root, ["--file", "zzz-nonexistent-zzz"])
        self.assertEqual(code, 2)

    def test_an_unknown_rule_exits_two(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "text\n")
            gate_tree.commit(root, "docs")
            code = self.run_main(root, ["--rule", "NOT-A-REAL-RULE"])
        self.assertEqual(code, 2)

    def test_rejects_unknown_arguments(self):
        self.assertEqual(check_style_audit.main(["--bogus"]), 2)


class ShellCompoundStatusTest(unittest.TestCase):
    rule_hits = StyleAuditTest.rule_hits

    def test_a_status_read_after_fi_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "tools/run.sh",
                            "#!/bin/sh\n"
                            "run_it() {\n"
                            "    if build >>\"$LOG\" 2>&1; then\n"
                            "        return 0\n"
                            "    fi\n"
                            "    rc=$?\n"
                            "    report \"$rc\"\n"
                            "}\n")
            gate_tree.commit(root, "tools")
            findings = self.rule_hits(root, "SHELL-COMPOUND-STATUS")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 6)
        self.assertIn("compound's status", findings[0].message)

    def test_exit_after_fi_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "tools/run.sh",
                            "#!/bin/sh\n"
                            "if report run; then\n"
                            "    exit 0\n"
                            "fi\n"
                            "exit $?\n")
            gate_tree.commit(root, "tools")
            findings = self.rule_hits(root, "SHELL-COMPOUND-STATUS")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 5)

    def test_the_capture_inside_the_branch_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "tools/run.sh",
                            "#!/bin/sh\n"
                            "run_it() {\n"
                            "    build >>\"$LOG\" 2>&1 && return 0\n"
                            "    rc=$?\n"
                            "    report \"$rc\"\n"
                            "}\n"
                            "report run || exit $?\n")
            gate_tree.commit(root, "tools")
            findings = self.rule_hits(root, "SHELL-COMPOUND-STATUS")
        self.assertEqual(findings, [])

    def test_a_c_file_is_not_shell(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/gfx.c",
                            "/* gfx - fixture header. */\n\n"
                            "void\ngfx_init(void) {\n}\n"
                            "int rc=$?\n")
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "SHELL-COMPOUND-STATUS")
        self.assertEqual(findings, [])


class LineEndingsTest(unittest.TestCase):
    rule_hits = StyleAuditTest.rule_hits

    def crlf_fixture(self, root):
        target = root / "tools" / "run.sh"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"#!/bin/sh\r\nset -eu\r\necho hello\r\n")
        gate_tree.commit(root, "tools")
        return target

    def test_a_crlf_working_copy_warns(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.crlf_fixture(root)
            findings = self.rule_hits(root, "LINE-ENDINGS")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].severity, "WARN")
        self.assertEqual(findings[0].line, 1)

    def test_fix_rewrites_it_as_lf_and_changes_nothing_else(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = self.crlf_fixture(root)
            findings, _ = check_style_audit.run_audit(root, rule_filter="LINE-ENDINGS")
            check_style_audit.run_fix(root, findings)
            self.assertEqual(target.read_bytes(), b"#!/bin/sh\nset -eu\necho hello\n")
            self.assertEqual(self.rule_hits(root, "LINE-ENDINGS"), [])

    def test_an_lf_file_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = root / "tools" / "run.sh"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"#!/bin/sh\nset -eu\n")
            gate_tree.commit(root, "tools")
            findings = self.rule_hits(root, "LINE-ENDINGS")
        self.assertEqual(findings, [])


if __name__ == "__main__":
    unittest.main()
