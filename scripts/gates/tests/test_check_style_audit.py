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

    def audit(self, rule_id, files, file_filter=None):
        """The rule's findings over a committed tree of `files`, {path: text}."""
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            for name, text in files.items():
                gate_tree.write(root, name, text)
            gate_tree.commit(root, *sorted({name.split("/")[0] for name in files}))
            return self.rule_hits(root, rule_id, file_filter)

    def test_pmu_owner(self):
        files = {
            "launcher/main/rogue.c": "int x = AXP2101_ROOT;\n",
            "launcher/main/input/rogue.c": "int x = BOARD_PMU_I2C_ADDR;\n",
            "launcher/main/selftest/rogue.h": "#define AXP2101_ROGUE 1\n",
            "launcher/main/power/owner.c": "int x = BOARD_PMU_I2C_ADDR;\n",
            "launcher/main/board/board.h": "#define BOARD_PMU_I2C_ADDR 0x34\n",
            "launcher/test/example.c": "int x = AXP2101_TEST;\n",
        }
        hits = self.audit("PMU-OWNER", files)
        self.assertEqual({hit.path for hit in hits}, {
            "launcher/main/rogue.c", "launcher/main/input/rogue.c", "launcher/main/selftest/rogue.h"})

    def test_pmu_owner_boundaries_and_comments(self):
        files = {
            "launcher/main/input/long.c": "int x = MY_BOARD_PMU_I2C_ADDR_COPY + MY_AXP2101_COPY;\n",
            "launcher/main/power/nested/owner.c": "int x = AXP2101_REGISTER;\n",
            "launcher/main/board/nested/owner.c": "int x = BOARD_PMU_I2C_ADDR;\n",
            "launcher/main/powerx/rogue.c": "int x = AXP2101_REGISTER;\n",
            "launcher/main/input/comment.c": "/* BOARD_PMU_I2C_ADDR belongs to the PMU. */\n",
        }
        self.assertEqual({hit.path for hit in self.audit("PMU-OWNER", files)},
                         {"launcher/main/powerx/rogue.c", "launcher/main/input/comment.c"})

    def test_power_include_tier(self):
        files = {
            "launcher/main/input/input.c": '#include "power/power.h"\n',
            "launcher/main/power/power.h": '#include "input/input.h"\n#include "audio/audio.h"\n',
            "launcher/main/input/input.h": "#pragma once\n",
            "launcher/main/audio/audio.h": "#pragma once\n",
        }
        self.assertEqual([(hit.path, hit.line) for hit in self.audit("INCLUDE-DIRECTION", files)],
                         [("launcher/main/power/power.h", 1), ("launcher/main/power/power.h", 2)])

    def test_undef_requires_analysis_scan_ifdef(self):
        hits = self.audit("UNDEF-PLACEMENT", {
            "launcher/main/gfx/defines.c": "#ifdef ANALYSIS_SCAN\n#undef A\n#ifdef INNER\n#undef B\n#endif\n"
                                           "#else\n#undef C\n#endif\n#undef D\n",
        })
        self.assertEqual([hit.line for hit in hits], [7, 9])

    def test_a_bd_issue_id_in_a_comment_is_flagged(self):
        findings = self.audit("TRACKER-REF", {
            "launcher/main/gfx/gfx.c": "/* gfx - fixture header. */\n\n"
                                       "void\ngfx_init(void) {\n"
                                       "    /* see bd autana-zq7 for the follow-up */\n}\n",
        })
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].severity, "ERROR")

    def test_a_bare_digit_led_tracker_id_is_flagged(self):
        findings = self.audit("TRACKER-REF", {"docs/Guide.md": "See autana-9qz for the follow-up.\n"})
        self.assertEqual(len(findings), 1)

    def test_a_full_commit_hash_in_a_doc_is_flagged(self):
        findings = self.audit("TRACKER-REF", {
            "docs/Guide.md": "Pinned to commit 8275e0af7c16aa40c54ea2b90b7af83b1fe4eb4c.\n",
        })
        self.assertEqual(len(findings), 1)

    def test_real_compound_words_and_a_short_build_id_are_not_flagged(self):
        # autana-cli, autana-screenshot, autana-monitor and autana-device
        # are all real, tracked, letter-only compounds in this repo; none
        # of them may start looking like a tracker id.
        findings = self.audit("TRACKER-REF", {
            "docs/Guide.md": "Run `autana screenshot` (autana-screenshot), `autana monitor` "
                             "(autana-monitor), autana-cli, or check autana-device; "
                             "build a195574e7177-diag.\n",
        })
        self.assertEqual(findings, [])

    def test_a_date_in_a_comment_is_flagged(self):
        findings = self.audit("CALENDAR-DATE", {
            "launcher/main/gfx/gfx.c": "/* gfx - fixture header. */\n\n"
                                       "void\ngfx_init(void) {\n"
                                       "    /* measured on device, 2026-09-13 */\n}\n",
        })
        self.assertEqual(len(findings), 1)

    def test_a_date_in_a_dated_log_txt_file_is_not_scanned(self):
        findings = self.audit("CALENDAR-DATE", {
            "scripts/gates/doc_review_ledger.txt": "docs/Guide.md\t2026-09-17\tchecked\n",
        })
        self.assertEqual(findings, [])

    def test_living_document_phrase_is_flagged(self):
        findings = self.audit("LIVING-DOC", {"docs/Guide.md": "This is a living document.\n"})
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
        findings = self.audit("INCLUDE-LAYER", {
            "launcher/main/gfx/fixed.h": "#pragma once\n",
            "launcher/main/apps/foo/fixed.h": "#pragma once\n",
            "launcher/main/apps/foo/app_foo.c": '#include "fixed.h"\n',
        })
        self.assertEqual(findings, [])

    def test_a_bare_include_that_resolves_nowhere_is_not_flagged(self):
        # Two layers each have a same-named header and neither sits at the
        # include root itself; the real compiler would not find this
        # include either, so it is a compile error, not this rule's concern.
        findings = self.audit("INCLUDE-LAYER", {
            "launcher/main/gfx/shared.h": "#pragma once\n",
            "launcher/main/ui/shared.h": "#pragma once\n",
            "launcher/main/apps/sand/app_sand.c": '#include "shared.h"\n',
        })
        self.assertEqual(findings, [])

    def test_a_relative_reach_into_board_is_flagged(self):
        # LAYER_DIRS used to be hand-typed and missed board/ entirely, so a
        # relative reach into it went unflagged.
        findings = self.audit("INCLUDE-LAYER", {
            "launcher/main/board/board.h": "#pragma once\n",
            "launcher/main/apps/sand/app_sand.c": '#include "../../board/board.h"\n',
        })
        self.assertEqual(len(findings), 1)
        self.assertIn("board/board.h", findings[0].message)

    def test_an_apps_internal_relative_include_is_not_flagged(self):
        findings = self.audit("INCLUDE-LAYER", {
            "launcher/main/apps/sand/material.h": "#pragma once\n",
            "launcher/main/apps/sand/ui/brush_screen.c": '#include "../material.h"\n',
        })
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
            gate_tree.write(root, "launcher/main/scene/scene.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/ui/ui.c", '#include "scene/scene.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(len(findings), 1)
        self.assertIn("tier", findings[0].message)

    def test_include_direction_judges_the_resolved_file_not_the_spelling(self):
        # A relative include from gfx/ into ui/ must be caught before
        # INCLUDE-LAYER's own --fix ever runs, since it reaches upward either way.
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

    def test_system_apps_use_the_app_layer(self):
        cases = (
            ("shell/shell.c", "shell/apps/x/x.h", 1),
            ("shell/apps/x/x.c", "apps/y/y.h", 0),
        )
        for source, target, expected in cases:
            with self.subTest(source=source):
                findings = self.audit("INCLUDE-DIRECTION", {
                    "launcher/main/" + source: f'#include "{target}"\n',
                    "launcher/main/" + target: "#pragma once\n",
                })
                self.assertEqual(len(findings), expected)

    def test_apps_own_sources_are_never_a_source_for_include_direction(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/core/tune.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/apps/foo/app_foo.c", '#include "core/tune.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(findings, [])

    def test_including_a_strictly_lower_tier_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/core/tune.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.c", '#include "core/tune.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(findings, [])

    def test_a_same_folder_include_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, "launcher/main/gfx/present/send/gfx_dirty.h", "#pragma once\n")
            gate_tree.write(root, "launcher/main/gfx/gfx.c", '#include "gfx/present/send/gfx_dirty.h"\n')
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(findings, [])

    def test_core_may_not_include_display_or_services(self):
        for target in ("display/display.h", "services/settings.h"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temp:
                root = pathlib.Path(temp)
                self.layer_tree(root)
                gate_tree.write(root, "launcher/main/" + target, "#pragma once\n")
                gate_tree.write(root, "launcher/main/core/device_state.c", f'#include "{target}"\n')
                gate_tree.commit(root, "launcher")
                findings = self.rule_hits(root, "INCLUDE-DIRECTION")
            self.assertEqual(len(findings), 1)

    def test_input_may_include_display_but_display_not_input_and_gfx_not_display(self):
        # Input maps gestures into the display viewport; display does not read input.
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
                         ["launcher/main/display/display_device.c", "launcher/main/gfx/gfx.c"])

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

    def assert_include_cases(self, rule_id, tree, flagged, allowed):
        """Each case is "source include": `source` holding only that include,
        over `tree`, draws one finding of `rule_id` when flagged and none when
        allowed. An include written <...> is kept angled; any other is quoted."""
        for cases, expected in ((flagged, 1), (allowed, 0)):
            for case in cases:
                source, include = case.split()
                with self.subTest(case=case):
                    line = f"#include {include}\n" if include.startswith("<") else f'#include "{include}"\n'
                    findings = self.audit(rule_id, {**tree, source: line})
                    self.assertEqual(len(findings), expected)

    def test_an_app_or_the_editor_including_the_system_band_is_flagged_but_not_the_app_contract(self):
        targets = ("shell/shell.h", "console/console_verbs.h", "selftest/post.h", "boot/boot_anim.h",
                   "app/app.h", "ui/ui.h")
        self.assert_include_cases("SYSTEM-BAND", {"launcher/main/" + t: "#pragma once\n" for t in targets}, flagged=(
            "launcher/main/apps/x/x.c shell/shell.h",
            "launcher/main/apps/x/x.h boot/boot_anim.h",
            "launcher/main/apps/x/tests/suite_x.c selftest/post.h",
            "launcher/main/apps/x/x.c <selftest/post.h>",
        ), allowed=(
            "launcher/main/apps/x/x.c app/app.h",
            "launcher/main/apps/x/x.c ui/ui.h",
            "launcher/main/apps/x/x.c console/console_verbs.h",
            "launcher/main/shell/apps/x/x.c selftest/post.h",
        ))

    def test_the_system_band_refused_follows_the_layer_table(self):
        system = dict(check_style_audit.LAYER_BANDS)["system"]
        between = {layer for row in system[1:-1] for layer in row.split()}
        self.assertEqual(check_style_audit.SYSTEM_BAND_REFUSED, between)
        self.assertNotIn(system[-1], between)

    BACKEND = {"launcher/components/microui/include/microui.h": "#pragma once\n"}

    def test_the_ui_backend_is_refused_outside_ui(self):
        self.assert_include_cases("UI-BACKEND", {
            **self.BACKEND,
            "launcher/main/ui/ui.h": "#pragma once\n",
            "launcher/main/ui/ui_internal.h": '#include "microui.h"\n',
            "launcher/main/ui/deep.h": '#include "ui/ui_internal.h"\n',
            "launcher/main/ui/deeper.h": '#include "ui/deep.h"\n',
            "launcher/main/ui/relative.h": '#include "../../components/microui/include/microui.h"\n',
            "launcher/test/probe.h": '#include "ui/ui_internal.h"\n',
        }, flagged=(
            "launcher/main/apps/x/x.c microui.h",
            "launcher/main/apps/x/x.c <microui.h>",
            "launcher/main/shell/x.c ../../components/microui/include/microui.h",
            "launcher/main/shell/x.c ui/relative.h",
            "launcher/main/shell/x.c ui/ui_internal.h",
            "launcher/main/shell/x.c ui/deeper.h",
            "launcher/main/apps/x/tests/suite_x.c probe.h",
        ), allowed=(
            "launcher/main/apps/x/x.c ui/ui.h",
            "launcher/main/ui/ui.c ui/ui_internal.h",
            "launcher/test/suites/suite_x.c ui/ui_internal.h",
            "launcher/tools/render/x.c ui/ui_internal.h",
        ))

    def test_a_leaking_header_outside_ui_is_reported_once_at_its_own_include(self):
        findings = self.audit("UI-BACKEND", {
            **self.BACKEND,
            "launcher/main/ui/ui_internal.h": '#include "microui.h"\n',
            "launcher/main/apps/x/leak.h": '#include "ui/ui_internal.h"\n',
            "launcher/main/apps/x/x.c": '#include "apps/x/leak.h"\n',
        })
        self.assertEqual([f.path for f in findings], ["launcher/main/apps/x/leak.h"])

    PACKAGE = "launcher/packages/math"

    def test_a_package_including_anything_outside_itself_is_flagged(self):
        # The firmware's layers, another package, and a relative climb out of
        # the package all count, quoted or angled.
        findings = self.audit("PACKAGE-INCLUDE", {
            "launcher/main/core/core.h": "#pragma once\n",
            "launcher/packages/other/include/other/other.h": "#pragma once\n",
            f"{self.PACKAGE}/include/math/scalar/fixed.h": '#include "core/core.h"\n#include <core/core.h>\n',
            f"{self.PACKAGE}/src/motion/orbit.c": '#include "../../../../main/core/core.h"\n',
            f"{self.PACKAGE}/tests/suite_fixed.c": '#include "other/other.h"\n',
        })
        self.assertEqual(sorted((f.path, f.line) for f in findings),
                         [(f"{self.PACKAGE}/include/math/scalar/fixed.h", 1),
                          (f"{self.PACKAGE}/include/math/scalar/fixed.h", 2),
                          (f"{self.PACKAGE}/src/motion/orbit.c", 1),
                          (f"{self.PACKAGE}/tests/suite_fixed.c", 1)])

    def test_a_package_may_include_itself_the_toolchain_the_build_config_and_the_test_harness(self):
        findings = self.audit("PACKAGE-INCLUDE", {
            "launcher/packages/testkit/include/testkit/suite.h": "#pragma once\n",
            f"{self.PACKAGE}/include/math/scalar/mathf.h": '#include <math.h>\n#include "sdkconfig.h"\n',
            f"{self.PACKAGE}/include/math/linear/vec3f.h": '#include "math/scalar/mathf.h"\n',
            f"{self.PACKAGE}/src/motion/orbit.c": '#include "math/linear/vec3f.h"\n',
            f"{self.PACKAGE}/tests/suite_vec3.c": '#include "testkit/suite.h"\n#include "unity.h"\n#include "math/linear/vec3f.h"\n',
        })
        self.assertEqual(findings, [])

    def test_only_package_tests_and_benches_may_include_testkit(self):
        findings = self.audit("PACKAGE-INCLUDE", {
            "launcher/packages/testkit/include/testkit/suite.h": "#pragma once\n",
            f"{self.PACKAGE}/tests/suite_x.c": '#include "testkit/suite.h"\n',
            f"{self.PACKAGE}/bench/measure.c": '#include "testkit/suite.h"\n',
            f"{self.PACKAGE}/src/x.c": '#include "testkit/suite.h"\n',
            f"{self.PACKAGE}/include/math/x.h": '#include "testkit/suite.h"\n',
        })
        self.assertEqual(sorted(f.path for f in findings),
                         [f"{self.PACKAGE}/include/math/x.h", f"{self.PACKAGE}/src/x.c"])

    def test_a_package_may_include_the_packages_it_declares_and_theirs_but_no_other(self):
        findings = self.audit("PACKAGE-INCLUDE", {
            "launcher/packages/physics/CMakeLists.txt": "set(physics_requires kinematics)\n",
            "launcher/packages/physics/include/physics/body.h":
                '#include "kinematics/follow.h"\n#include "math/scalar/mathf.h"\n#include "name_hash/name_hash.h"\n',
            "launcher/packages/kinematics/CMakeLists.txt": "set(kinematics_requires math)\n",
            "launcher/packages/kinematics/include/kinematics/follow.h": '#include "physics/body.h"\n',
            "launcher/packages/name_hash/include/name_hash/name_hash.h": "#pragma once\n",
            f"{self.PACKAGE}/include/math/scalar/mathf.h": "#pragma once\n",
        })
        self.assertEqual(sorted((f.path, f.line) for f in findings),
                         [("launcher/packages/kinematics/include/kinematics/follow.h", 1),
                          ("launcher/packages/physics/include/physics/body.h", 3)])

    def test_package_requires_names_real_packages_one_way(self):
        findings = self.audit("PACKAGE-REQUIRES", {
            "launcher/packages/physics/CMakeLists.txt": "# physics\nset(physics_requires math physics nowhere)\n",
            "launcher/packages/physics/include/physics/body.h": "#pragma once\n",
            "launcher/packages/kinematics/CMakeLists.txt": "set(kinematics_requires math)\n",
            "launcher/packages/kinematics/include/kinematics/follow.h": "#pragma once\n",
            f"{self.PACKAGE}/CMakeLists.txt": "set(math_requires collections)\n",
            f"{self.PACKAGE}/include/math/scalar/mathf.h": "#pragma once\n",
            "launcher/packages/collections/CMakeLists.txt": "set(collections_requires kinematics)\n",
            "launcher/packages/collections/include/collections/list.h": "#pragma once\n",
        })
        messages = sorted((f.path, f.line, f.message.split(",")[0]) for f in findings)
        self.assertEqual(messages, [
            ("launcher/packages/collections/CMakeLists.txt", 1, "collections requires kinematics"),
            ("launcher/packages/kinematics/CMakeLists.txt", 1, "kinematics requires math"),
            (f"{self.PACKAGE}/CMakeLists.txt", 1, "math requires collections"),
            ("launcher/packages/physics/CMakeLists.txt", 2, "physics requires itself"),
            ("launcher/packages/physics/CMakeLists.txt", 2, "physics requires nowhere"),
        ])

    def test_package_requires_refuses_a_declaration_it_cannot_read(self):
        for text in ('set(physics_requires "math")\n', "set(physics_requires math-lite)\n",
                     "set(physics_requires ${X})\n", "set(physics_requires\n    math)\n",
                     "if(ESP_PLATFORM)\n    set(physics_requires math)\nendif()\n",
                     "set(physics_requires math) # trailing\n"):
            with self.subTest(text=text):
                findings = self.audit("PACKAGE-REQUIRES", {
                    "launcher/packages/physics/CMakeLists.txt": text,
                    "launcher/packages/physics/include/physics/body.h": "#pragma once\n",
                    f"{self.PACKAGE}/include/math/scalar/mathf.h": "#pragma once\n",
                })
                self.assertEqual(len(findings), 1, findings)
                self.assertIn("one unindented line of bare package names", findings[0].message)

    def test_package_requires_passes_a_one_way_graph_and_ignores_nested_lists(self):
        findings = self.audit("PACKAGE-REQUIRES", {
            "launcher/packages/physics/CMakeLists.txt": "set(physics_requires math kinematics)\n",
            "launcher/packages/physics/include/physics/body.h": "#pragma once\n",
            "launcher/packages/physics/tests/CMakeLists.txt": "set(physics_requires physics)\n",
            "launcher/packages/kinematics/CMakeLists.txt": "set(kinematics_requires math)\n",
            "launcher/packages/kinematics/include/kinematics/follow.h": "#pragma once\n",
            f"{self.PACKAGE}/include/math/scalar/mathf.h": "#pragma once\n",
        })
        self.assertEqual(findings, [])

    def test_package_config_rejects_product_and_other_package_symbols(self):
        for name in ("probe.c", "include/math/probe.h", "CMakeLists.txt", "Kconfig"):
            for symbol in ("CONFIG_LAUNCHER_QEMU", "CONFIG_OTHER_FPU_RECIP"):
                with self.subTest(name=name, symbol=symbol):
                    findings = self.audit("PACKAGE-CONFIG", {
                        f"{self.PACKAGE}/{name}": f"\nif({symbol})\n",
                        "launcher/main/Kconfig.projbuild": "config LAUNCHER_QEMU\n",
                        "launcher/packages/other/Kconfig": "config OTHER_FPU_RECIP\n",
                    })
                    self.assertEqual([f.line for f in findings], [2])
                    self.assertIn(symbol, findings[0].message)

    def test_package_config_rejects_undeclared_own_symbols(self):
        for name in ("probe.c", "include/math/probe.h", "CMakeLists.txt", "Kconfig"):
            with self.subTest(name=name):
                findings = self.audit("PACKAGE-CONFIG", {
                    f"{self.PACKAGE}/{name}": "\nif(CONFIG_MATH_FPU_RECP)\n",
                    f"{self.PACKAGE}/Kconfig.extra": "config MATH_FPU_RECIP\n",
                })
                self.assertEqual([f.line for f in findings], [2])
                self.assertIn("CONFIG_MATH_FPU_RECP", findings[0].message)

    def test_package_config_accepts_idf_platform_symbols(self):
        findings = self.audit("PACKAGE-CONFIG", {
            f"{self.PACKAGE}/probe.c": "#if CONFIG_IDF_TARGET_ARCH_XTENSA\n#endif\n"
                                      "#if CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE\n#endif\n",
            "launcher/main/Kconfig.projbuild": "config LAUNCHER_QEMU\n",
        })
        self.assertEqual(findings, [])

    def test_package_config_accepts_its_own_prefix(self):
        findings = self.audit("PACKAGE-CONFIG", {
            f"{self.PACKAGE}/probe.c": "#if CONFIG_MATH_FPU_RECIP\n#endif\n",
            f"{self.PACKAGE}/include/math/probe.h": "#ifdef CONFIG_MATH_FPU_RECIP\n#endif\n",
            f"{self.PACKAGE}/CMakeLists.txt": "if(CONFIG_MATH_FPU_RECIP)\nendif()\n",
            f"{self.PACKAGE}/Kconfig": "config MATH_FPU_RECIP\n    default y\n",
            "launcher/packages/other/probe.c": "#if CONFIG_OTHER_FEATURE\n#endif\n",
            "launcher/packages/other/Kconfig": "config OTHER_FEATURE\n",
            "launcher/main/probe.c": "#if CONFIG_LAUNCHER_QEMU\n#endif\n",
        })
        self.assertEqual(findings, [])

    def test_package_kconfig_declarations_require_its_prefix(self):
        findings = self.audit("PACKAGE-CONFIG", {
            f"{self.PACKAGE}/Kconfig": "config LAUNCHER_FEATURE\nconfig OTHER_FEATURE\n"
                                      "menuconfig MATH_FEATURE\nmenuconfig FOREIGN_FEATURE\n",
        })
        self.assertEqual([f.line for f in findings], [1, 2, 4])

    def test_package_config_ignores_comments_but_checks_code_on_the_same_line(self):
        findings = self.audit("PACKAGE-CONFIG", {
            "launcher/main/Kconfig.projbuild": "config LAUNCHER_QEMU\n",
            f"{self.PACKAGE}/probe.c": "/* CONFIG_UNITY_ENABLE_64BIT */\n"
                                      "// CONFIG_LAUNCHER_QEMU\n"
                                      "/* CONFIG_OTHER_FEATURE */ #if CONFIG_LAUNCHER_QEMU\n",
            f"{self.PACKAGE}/CMakeLists.txt": "# CONFIG_LAUNCHER_QEMU\n"
                                             "#[=[\nCONFIG_OTHER_FEATURE\n]=]\n"
                                             "if(CONFIG_MATH_FPU_RECIP) # CONFIG_OTHER_FEATURE\n",
            f"{self.PACKAGE}/Kconfig": "config MATH_FPU_RECIP\n# config OTHER_FEATURE CONFIG_OTHER_FEATURE\n"
                                      "config MATH_FEATURE # CONFIG_LAUNCHER_QEMU\n",
        })
        self.assertEqual([(f.path, f.line) for f in findings], [(f"{self.PACKAGE}/probe.c", 3)])

    def test_package_config_checks_symbols_in_build_strings(self):
        findings = self.audit("PACKAGE-CONFIG", {
            f"{self.PACKAGE}/CMakeLists.txt": 'message("# ${CONFIG_OTHER_FEATURE}")\n',
            "launcher/packages/other/Kconfig": "config OTHER_FEATURE\n",
        })
        self.assertEqual([f.line for f in findings], [1])

    def test_a_package_header_is_spelled_from_its_include_root_inside_the_package_too(self):
        findings = self.audit("INCLUDE-LAYER", {
            f"{self.PACKAGE}/include/math/scalar/mathf.h": "#pragma once\n",
            f"{self.PACKAGE}/include/math/scalar/fixed.h": '#include "mathf.h"\n',
            f"{self.PACKAGE}/src/fixed.c": '#include "../include/math/scalar/mathf.h"\n',
        })
        self.assertEqual(sorted((f.path, f.message) for f in findings),
                         [(f"{self.PACKAGE}/include/math/scalar/fixed.h",
                           '"mathf.h" is not layer-qualified - use "math/scalar/mathf.h"'),
                          (f"{self.PACKAGE}/src/fixed.c",
                           '"../include/math/scalar/mathf.h" is not layer-qualified - use "math/scalar/mathf.h"')])

    def test_every_layer_may_include_a_package(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            gate_tree.write(root, f"{self.PACKAGE}/include/math/scalar/fixed.h", "#pragma once\n")
            for layer in check_style_audit.LAYER_DIRS:
                gate_tree.write(root, f"launcher/main/{layer}/probe.c", '#include "math/scalar/fixed.h"\n')
            gate_tree.commit(root, "launcher")
            self.assertEqual(self.rule_hits(root, "INCLUDE-DIRECTION"), [])
            self.assertEqual(self.rule_hits(root, "INCLUDE-LAYER"), [])

    BASE_ROWS = (("services",), ("profile",), ("core",), ("board",), ("build",))

    def test_each_base_layer_includes_only_the_rows_below_it(self):
        # One header per layer, and from each layer one file including every other; only
        # the includes that reach upward or into the same row of BASE_ROWS are flagged.
        layers = [layer for row in self.BASE_ROWS for layer in row]
        row_of = {layer: i for i, row in enumerate(self.BASE_ROWS) for layer in row}
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            for layer in layers:
                gate_tree.write(root, f"launcher/main/{layer}/{layer}.h", "#pragma once\n")
            for layer in layers:
                others = "".join(f'#include "{other}/{other}.h"\n' for other in layers if other != layer)
                gate_tree.write(root, f"launcher/main/{layer}/probe.c", others)
            gate_tree.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        expected = []
        for layer in layers:
            others = [other for other in layers if other != layer]
            expected += [(f"launcher/main/{layer}/probe.c", line)
                         for line, other in enumerate(others, 1) if row_of[other] <= row_of[layer]]
        self.assertEqual(sorted((f.path, f.line) for f in findings), sorted(expected))

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
        findings = self.audit("PERSONAL-PATH", {
            "scripts/device/device.py": 'IDF_PYTHON = r"C:\\Users\\jdoe\\.espressif\\python_env\\python.exe"\n',
        })
        self.assertEqual(len(findings), 1)

    def test_a_drive_rooted_esp_idf_checkout_is_flagged(self):
        findings = self.audit("PERSONAL-PATH", {
            "launcher/tools/build.sh": "DEFAULT_EXPORT='" + "\\".join(["C:", "Espressif", "esp-idf-v5.5", "export.bat"]) + "'\n",
        })
        self.assertEqual(len(findings), 1)

    def test_any_drive_path_to_an_esp_idf_checkout_is_flagged(self):
        # The layouts an installer actually produces, not only one machine's.
        layouts = ["\\".join(["C:", "Espressif", "v5.5.5", "esp-idf", "export.bat"]),
                   "\\".join(["D:", "esp", "esp-idf", "export.bat"]),
                   "/".join(["C:", "Espressif", "v5.5.5", "esp-idf"])]
        findings = self.audit("PERSONAL-PATH", {
            "launcher/tools/build.sh": "".join(f"EXPORT_{i}='{layout}'\n" for i, layout in enumerate(layouts)),
        })
        self.assertEqual(len(findings), len(layouts))

    def test_portable_esp_idf_references_are_not_flagged(self):
        findings = self.audit("PERSONAL-PATH", {
            "launcher/tools/build.sh": 'EXPORT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"\n'
                                       "# https://github.com/espressif/esp-idf\n",
        })
        self.assertEqual(findings, [])

    def test_a_portable_home_relative_path_is_not_flagged(self):
        findings = self.audit("PERSONAL-PATH", {
            "scripts/device/device.py": 'GLOB = "~/.espressif/python_env/idf*_env/Scripts/python.exe"\n',
        })
        self.assertEqual(findings, [])

    def test_utf8_read_as_cp1252_is_flagged(self):
        mangled = {"middle dot": "\u00b7", "arrow": "\u2192", "e acute": "\u00e9", "check": "\u2713"}
        text = "".join(f"{name}: {char.encode('utf-8').decode('cp1252', errors='replace')}\n"
                       for name, char in mangled.items())
        findings = self.audit("MOJIBAKE", {"docs/page.md": text})
        self.assertEqual(len(findings), len(mangled))

    def test_real_accents_and_symbols_are_not_flagged(self):
        findings = self.audit("MOJIBAKE", {
            "docs/page.md": "caf\u00e9 \u00c0 la carte, \u00a9, 25\u00b0C, \u00b7 \u2192 \u2713, na\u00efve \u00c9cole\n",
        })
        self.assertEqual(findings, [])

    def test_mojibake_fix_restores_the_character(self):
        mangled = "direct \u00c2\u00b7 occlusion \u00e2\u2020\u2019 bounce"
        self.assertEqual(check_style_audit._fix_mojibake(mangled), "direct \u00b7 occlusion \u2192 bounce")

    def test_a_tool_named_after_an_app_model_is_flagged(self):
        findings = self.audit("MODEL-NAMED-TOOL", {
            "launcher/main/apps/lab/assets/Otter.blend": "model",
            "launcher/main/apps/lab/tools/export_otter_glb.py": "pass\n",
            "scripts/otter_bake.sh": "true\n",
        })
        self.assertEqual(sorted(f.path for f in findings),
                         ["launcher/main/apps/lab/tools/export_otter_glb.py", "scripts/otter_bake.sh"])

    def test_a_model_outside_the_apps_names_tools_too(self):
        findings = self.audit("MODEL-NAMED-TOOL", {
            "launcher/demo/otter/otter.glb": "model",
            "launcher/tools/r3d/otter_lighting.py": "pass\n",
        })
        self.assertEqual([f.path for f in findings], ["launcher/tools/r3d/otter_lighting.py"])

    def test_app_runtime_code_and_generic_tools_may_name_or_skip_models(self):
        findings = self.audit("MODEL-NAMED-TOOL", {
            "launcher/main/apps/lab/assets/otter.glb": "model",
            "launcher/main/apps/lab/scene_otter.c": "int x;\n",
            "launcher/tools/gltf/blend_skin_to_glb.py": "pass\n",
            "docs/images/otter.png": "image",
        })
        self.assertEqual(findings, [])

    def test_a_stray_html_comment_is_flagged(self):
        findings = self.audit("STRAY-HTML-COMMENT", {
            "docs/Guide.md": "text\n<!-- TODO: remove this -->\nmore text\n",
        })
        self.assertEqual(len(findings), 1)

    def test_a_multi_line_html_comment_is_flagged(self):
        findings = self.audit("STRAY-HTML-COMMENT", {
            "docs/Guide.md": "text\n<!-- a stray aside\nspanning two lines -->\nmore\n",
        })
        self.assertEqual(len(findings), 1)

    def test_a_known_escape_marker_is_not_flagged(self):
        findings = self.audit("STRAY-HTML-COMMENT", {
            "docs/Guide.md": "<!-- BEGIN GENERATED -->\ntable\n<!-- END GENERATED -->\n",
        })
        self.assertEqual(findings, [])

    def test_a_named_generated_block_is_not_flagged(self):
        findings = self.audit("STRAY-HTML-COMMENT", {
            "docs/Guide.md": "<!-- generated: scores sha256=" + "0" * 64 + " -->\n| 2 |\n"
                             "<!-- /generated: scores -->\n<!-- generated: notes -->\n"
                             "<!-- generated: table check: python gen.py --check -->\n",
        })
        self.assertEqual(findings, [])

    def test_malformed_generated_marker_is_stray(self):
        lines = ["<!-- generated: Bad_Name -->"]
        self.assertEqual(1, len(list(check_style_audit.rule_stray_html_comment(
            pathlib.Path("."), "doc.md", lines))))

    def test_a_bullet_right_after_unindented_prose_is_flagged(self):
        findings = self.audit("ACCIDENTAL-BULLET", {
            "docs/Guide.md": "This sentence continues onto the next line where it\n"
                             "- 3 happens to start with a dash and a space\n",
        })
        self.assertEqual(len(findings), 1)

    def test_a_bullet_after_an_indented_list_continuation_is_not_flagged(self):
        findings = self.audit("ACCIDENTAL-BULLET", {
            "docs/Guide.md": "- **First item.** Some wrapped continuation text\n"
                             "  that is indented under the item above.\n"
                             "- Second item.\n",
        })
        self.assertEqual(findings, [])

    def test_a_lazily_continued_list_item_is_not_flagged(self):
        # CommonMark's lazy continuation lets a list item's paragraph carry
        # on with no indentation at all; the paragraph this "- " sits in
        # started with its own list marker, so it is a sibling item, not an
        # accident.
        findings = self.audit("ACCIDENTAL-BULLET", {
            "docs/Guide.md": "- First item text\n"
                             "continues here with no indentation at all\n"
                             "- Second item\n",
        })
        self.assertEqual(findings, [])

    def test_a_list_right_after_a_closing_fence_is_not_flagged(self):
        findings = self.audit("ACCIDENTAL-BULLET", {
            "docs/Guide.md": "```sh\nexample\n```\n- a real list item\n",
        })
        self.assertEqual(findings, [])

    def test_a_real_list_introduced_by_a_colon_is_not_flagged(self):
        findings = self.audit("ACCIDENTAL-BULLET", {"docs/Guide.md": "The rules are:\n- one\n- two\n"})
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
        findings = self.audit("BLANK-LINES", {
            "docs/Table.md": "<!-- BEGIN GENERATED -->\na\n\n\nb\n<!-- END GENERATED -->\n",
        })
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
        findings = self.audit("HEADING-COMMENT", {
            "launcher/main/gfx/gfx.c": "/* gfx - fixture header. */\n\n"
                                       "void\nfoo(void) {\n    int a = 1;\n    /* Setup */\n    a = 2;\n}\n",
        })
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].severity, "WARN")

    def test_the_same_text_as_a_file_level_divider_is_not_flagged(self):
        findings = self.audit("HEADING-COMMENT", {
            "launcher/main/gfx/gfx.c": "/* Setup */\n\nstatic int\nfoo(void) {\n    return 1;\n}\n",
        })
        self.assertEqual(findings, [])

    def test_the_same_text_inside_an_array_initializer_is_not_flagged(self):
        findings = self.audit("HEADING-COMMENT", {
            "launcher/main/gfx/gfx.c": "static const int table[] = {\n    /* Values */\n    1, 2, 3,\n};\n",
        })
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

    def include_direction_beside(self, files):
        """The INCLUDE-DIRECTION exit code over one gfx source and `files`."""
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/gfx/gfx.c", "int x;\n")
            for path, text in files.items():
                gate_tree.write(root, path, text)
            gate_tree.commit(root, "launcher")
            return self.run_main(root, ["--rule", "INCLUDE-DIRECTION"])

    def test_an_unknown_layer_folder_exits_two_instead_of_crashing(self):
        self.assertEqual(self.include_direction_beside({"launcher/main/newlayer/thing.h": "#pragma once\n"}), 2)

    def test_a_folder_of_content_alone_has_no_tier(self):
        self.assertEqual(self.include_direction_beside({"launcher/main/content/content.pack.toml": "",
                                                        "launcher/main/content/icons/set.icons.toml": ""}), 0)

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
    audit = StyleAuditTest.audit

    def test_a_status_read_after_fi_is_flagged(self):
        findings = self.audit("SHELL-COMPOUND-STATUS", {
            "tools/run.sh": "#!/bin/sh\n"
                            "run_it() {\n"
                            "    if build >>\"$LOG\" 2>&1; then\n"
                            "        return 0\n"
                            "    fi\n"
                            "    rc=$?\n"
                            "    report \"$rc\"\n"
                            "}\n",
        })
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 6)
        self.assertIn("compound's status", findings[0].message)

    def test_exit_after_fi_is_flagged(self):
        findings = self.audit("SHELL-COMPOUND-STATUS", {
            "tools/run.sh": "#!/bin/sh\n"
                            "if report run; then\n"
                            "    exit 0\n"
                            "fi\n"
                            "exit $?\n",
        })
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 5)

    def test_the_capture_inside_the_branch_is_not_flagged(self):
        findings = self.audit("SHELL-COMPOUND-STATUS", {
            "tools/run.sh": "#!/bin/sh\n"
                            "run_it() {\n"
                            "    build >>\"$LOG\" 2>&1 && return 0\n"
                            "    rc=$?\n"
                            "    report \"$rc\"\n"
                            "}\n"
                            "report run || exit $?\n",
        })
        self.assertEqual(findings, [])

    def test_a_c_file_is_not_shell(self):
        findings = self.audit("SHELL-COMPOUND-STATUS", {
            "launcher/main/gfx/gfx.c": "/* gfx - fixture header. */\n\n"
                                       "void\ngfx_init(void) {\n}\n"
                                       "int rc=$?\n",
        })
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
