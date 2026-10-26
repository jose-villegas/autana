"""check_package_docs.py: the page tracks the headers, in both directions."""

import pathlib
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import check_package_docs  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
FUNCTION = "    static inline int P##_add(int a) { return a; } \\\n"


def make_tree(root, page_extra="", template_extra="", header_extra="", skip_section=None, empty_template=None,
              scalar_extra=""):
    """A minimal tree that agrees with itself; each argument breaks one rule."""
    math = root / "launcher/packages/math/include/math/linear"
    math.mkdir(parents=True)
    (root / "docs/math").mkdir(parents=True)
    for family in check_package_docs.FAMILIES:
        body = "#define MATH_DEFINE_X(P) \\\n" + FUNCTION + (template_extra if family == "vec2" else "")
        if family == empty_template:
            body = "#define MATH_DEFINE_X(P)\n"
        (root / check_package_docs.FAMILY_DIRS[family]).mkdir(parents=True, exist_ok=True)
        (root / check_package_docs.FAMILY_DIRS[family] / (family + "_template.h")).write_text(("typedef struct { int x; } P##_t;\n" + body).replace("MATH_DEFINE_X", "MATH_DEFINE_" + family.upper()))
    headers = math.parent / "scalar"
    headers.mkdir()
    (headers / "helpers.h").write_text(scalar_extra)
    (math / "vec2f.h").write_text("MATH_DEFINE_VEC2(vec2f, float)\n" + header_extra)
    page = "".join("### %s\n\n| `P_add(` |\n\n" % f for f in check_package_docs.FAMILIES if f != skip_section)
    page += "`vec2f`\n" + page_extra
    write_pages(root, page)


def write_pages(root, page):
    body, marker, extra = page.partition("\n`vec2f`")
    sections = {}
    for family, directory in check_package_docs.FAMILY_DIRS.items():
        path = check_package_docs.family_page(directory)
        sections.setdefault(path, []).append(check_package_docs.section(body, family))
    for path, bodies in sections.items():
        (root / path).write_text("\n".join(bodies))
    pages = [p.name for p in sections] + ["Scalar.md"]
    (root / check_package_docs.PAGE.parent / "Scalar.md").write_text("")
    (root / check_package_docs.PAGE).write_text(
        "\n".join("[page](%s)" % name for name in pages) + "\n" + marker + extra)


def problems_for(**kwargs):
    with tempfile.TemporaryDirectory() as d:
        make_tree(pathlib.Path(d), **kwargs)
        return check_package_docs.check(d)


class CheckMathDocs(unittest.TestCase):
    def test_family_sections_accept_both_heading_levels(self):
        for heading in ("##", "###"):
            with self.subTest(heading=heading):
                text = heading + " vec2 (`f i s x`)\n\n`P_add(a)`\n\n## vec3\n`P_other(a)`\n"
                body = check_package_docs.section(text, "vec2")
                self.assertIn("`P_add(a)`", body)
                self.assertNotIn("P_other", body)

    def test_family_must_be_on_its_folder_page(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root)
            source = root / check_package_docs.MATH
            target = source.parent / "curves"
            target.mkdir()
            for name in ("vec2_template.h", "vec2f.h"):
                (source / name).rename(target / name)
            problems = check_package_docs.check(root)
            self.assertTrue(any("Curves.md has no family section for `vec2`" in p for p in problems))
            page = root / "docs/math/Curves.md"
            page.write_text(check_package_docs.section((root / "docs/math/Linear.md").read_text(), "vec2"))
            index = root / check_package_docs.PAGE
            index.write_text(index.read_text() + "\n[curves](Curves.md)\n")
            self.assertEqual([], check_package_docs.check(root))

    def test_real_headers_and_pages_require_declarations_and_generated_functions(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            for directory in (check_package_docs.MATH.parent, check_package_docs.PAGE.parent):
                shutil.copytree(REPO / directory, root / directory)
            page = root / "docs/math/Projection.md"
            original = page.read_text()
            for name in ("projectf_clip_near", "projectx_clip_near"):
                with self.subTest(name=name):
                    page.write_text(original.replace(name, "omitted"))
                    self.assertTrue(any(name in p for p in check_package_docs.check(root)))
            page.write_text(original)
            self.assertEqual([], check_package_docs.check(root))

    def test_non_family_instantiation_functions_are_required_on_any_page(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root)
            directory = root / "launcher/packages/math/include/math/projection"
            directory.mkdir()
            (directory / "clip_template.h").write_text(
                "#define MATH_DEFINE_CLIP_NEAR(P) MATH_CLIP(P, clip_near)\n"
                "#define MATH_CLIP(P, OP) static inline int P##_##OP(int n) { return n; }\n")
            (directory / "projectf.h").write_text("MATH_DEFINE_CLIP_NEAR(projectf)\n")
            self.assertTrue(any("projectf_clip_near" in p for p in check_package_docs.check(root)))
            page = root / "docs/math/Projection.md"
            page.write_text("`projectf_clip_near(n)`\n")
            index = root / check_package_docs.PAGE
            index.write_text(index.read_text() + "\n[projection](Projection.md)\n")
            self.assertEqual([], check_package_docs.check(root))
            index.write_text(index.read_text() + page.read_text())
            page.write_text("")
            self.assertEqual([], check_package_docs.check(root))

    def test_inline_math_limit_reports_each_page(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root)
            for page in (root / "docs/math").glob("*.md"):
                original = page.read_text()
                page.write_text(original + "\n$" + "x" * (check_package_docs.MAX_INLINE_MATH_CHARS + 1) + "$\n")
                self.assertTrue(any(p.startswith(page.relative_to(root).as_posix() + ":")
                                    and "inline math" in p for p in check_package_docs.check(root)))
                page.write_text(original)

    def test_index_must_link_every_page(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root)
            (root / "docs/math/Extra.md").write_text("# Extra\n")
            self.assertTrue(any("index" in p and "Extra.md" in p for p in check_package_docs.check(root)))
            index = root / check_package_docs.PAGE
            index.write_text(index.read_text() + "\n[extra](Extra.md)\n")
            self.assertEqual([], check_package_docs.check(root))


    def test_the_real_page_matches_the_real_headers(self):
        self.assertEqual([], check_package_docs.check(REPO))

    def test_geometry_template_functions_and_types_are_checked(self):
        for family, function in (("rect", "right"), ("plane", "distance")):
            with self.subTest(family=family), tempfile.TemporaryDirectory() as d:
                root = pathlib.Path(d)
                make_tree(root)
                geometry = root / "launcher/packages/math/include/math/geometry"
                geometry.mkdir(exist_ok=True)
                (geometry / (family + "_template.h")).write_text(
                    "typedef struct { int x; } P##_t;\n#define MATH_DEFINE_%s(P, T, OPS) \\\n" % family.upper()
                    + "    static inline T P##_%s(P##_t r) { return r.x; }\n" % function
                )
                prefix = family + "f"
                (geometry / (prefix + ".h")).write_text(
                    "MATH_DEFINE_%s(%s, float, mathf)\n" % (family.upper(), prefix))
                page = root / "docs/math/Geometry.md"
                text = page.read_text()
                section = "### %s\n\n| `P_add(` |\n\n" % family
                entry = "`P_%s(r)`" % function
                page.write_text(text.replace(section, "") + "### %s\n\n| %s |\n`%s`\n" % (family, entry, prefix))
                self.assertEqual([], check_package_docs.check(root))
                page.write_text(page.read_text().replace(entry, ""))
                self.assertTrue(any("%s section lacks `P_%s(" % (family, function) in p
                                    for p in check_package_docs.check(root)))
                page.write_text(page.read_text().replace("`%s`" % prefix, entry))
                self.assertTrue(any("does not name the type `%s`" % prefix in p
                                    for p in check_package_docs.check(root)))

    def test_a_synthetic_tree_that_agrees_passes(self):
        self.assertEqual([], problems_for())

    def test_a_short_inline_math_span_passes(self):
        self.assertEqual([], problems_for(page_extra="$`a + b`$\n"))

    def test_an_inline_math_span_at_the_limit_passes(self):
        formula = "x" * check_package_docs.MAX_INLINE_MATH_CHARS
        self.assertEqual([], problems_for(page_extra="$`%s`$\n" % formula))

    def test_an_over_limit_inline_math_span_fails_with_its_line(self):
        formula = "x" * (check_package_docs.MAX_INLINE_MATH_CHARS + 1)
        for prefix in ("\n", "```math\na + b\nc + d\n```\n"):
            with self.subTest(prefix=prefix):
                with tempfile.TemporaryDirectory() as d:
                    root = pathlib.Path(d)
                    make_tree(root, page_extra=prefix + "$`%s`$\n" % formula)
                    page = (root / check_package_docs.PAGE).read_text()
                    expected_line = next(i for i, line in enumerate(page.splitlines(), start=1) if formula in line)
                    problems = check_package_docs.check(root)
                self.assertEqual(1, len(problems))
                self.assertIn("docs/math/README.md:%d:" % expected_line, problems[0])
                self.assertIn("split the formula into one span per step joined with <br>", problems[0])

    def test_separate_inline_math_spans_are_measured_individually(self):
        formula = "x" * check_package_docs.MAX_INLINE_MATH_CHARS
        self.assertEqual([], problems_for(page_extra="$`%s`$<br> $`%s`$\n" % (formula, formula)))

    def test_every_inline_math_span_on_a_line_is_measured(self):
        for delimiters in (("$`", "`$"), ("$", "$")):
            with self.subTest(delimiters=delimiters):
                opening, closing = delimiters
                for length in (check_package_docs.MAX_INLINE_MATH_CHARS, check_package_docs.MAX_INLINE_MATH_CHARS + 1):
                    with self.subTest(length=length):
                        formula = "x" * length
                        problems = problems_for(page_extra="%sa%s then %s%s%s\n" %
                                                (opening, closing, opening, formula, closing))
                        self.assertEqual(int(length > check_package_docs.MAX_INLINE_MATH_CHARS), len(problems))

    def test_an_over_limit_plain_dollar_math_span_fails(self):
        formula = "x" * (check_package_docs.MAX_INLINE_MATH_CHARS + 1)
        problems = problems_for(page_extra="$%s$\n" % formula)
        self.assertEqual(1, len(problems))
        self.assertIn("inline math has %d characters" % len(formula), problems[0])

    def test_dollars_inside_code_spans_are_not_measured(self):
        formula = "x" * (check_package_docs.MAX_INLINE_MATH_CHARS + 1)
        for code in ("`$%s$`", "``$`%s`$``"):
            with self.subTest(code=code):
                self.assertEqual([], problems_for(page_extra=code % formula + "\n"))

    def test_inline_math_inside_code_fences_is_not_measured(self):
        formula = "x" * (check_package_docs.MAX_INLINE_MATH_CHARS + 1)
        for language in ("math", "c"):
            with self.subTest(language=language):
                self.assertEqual([], problems_for(page_extra="```%s\n$%s$ and $`%s`$\n```\n" %
                                                 (language, formula, formula)))

    def test_escaped_dollars_are_not_measured(self):
        formula = "x" * (check_package_docs.MAX_INLINE_MATH_CHARS + 1)
        self.assertEqual([], problems_for(page_extra=r"\$%s\$" % formula + "\n"))

    def test_display_dollar_math_is_not_measured(self):
        formula = "x" * (check_package_docs.MAX_INLINE_MATH_CHARS + 1)
        for display in ("$$%s$$", r"$$\verb|$|%s$$"):
            with self.subTest(display=display):
                self.assertEqual([], problems_for(page_extra=display % formula + "\n"))

    def test_a_template_function_missing_from_the_page_fails(self):
        extra = "    static inline int P##_wobble(int a) { return a; } \\\n"
        self.assertTrue(any("P_wobble(" in p for p in problems_for(template_extra=extra)))

    def test_a_forced_inline_template_function_missing_from_the_page_fails(self):
        extra = "    static inline __attribute__((always_inline)) int P##_wobble(int a) { return a; } \\\n"
        self.assertTrue(any("P_wobble(" in p for p in problems_for(template_extra=extra)))

    def test_nested_definers_bind_renamed_operation_parameters(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root, template_extra="    MATH_FORWARD(P, wobble)\n")
            math = root / check_package_docs.MATH
            (math / "vec_ops_template.h").write_text(
                "#define MATH_FORWARD(P, NAME) MATH_OP(P, NAME)\n"
                "#define MATH_OP(P, OP) static inline int P##_##OP(int a) { return a; }\n"
            )
            self.assertEqual(
                ["docs/math/Linear.md: the vec2 section lacks `P_wobble(`"],
                check_package_docs.check(root),
            )
            page = root / "docs/math/Linear.md"
            page.write_text(page.read_text().replace("### vec2\n", "### vec2\n`P_wobble(a)`\n"))
            self.assertEqual([], check_package_docs.check(root))

    def test_recursive_definers_stop_at_a_cycle(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root, template_extra="    MATH_CYCLE(P)\n")
            (root / check_package_docs.MATH / "vec_ops_template.h").write_text(
                "#define MATH_CYCLE(P) MATH_BACK(P)\n"
                "#define MATH_BACK(P) MATH_CYCLE(P)\n"
            )
            self.assertEqual([], check_package_docs.check(root))

    def test_a_hand_written_function_the_page_lacks_fails(self):
        extra = "static inline int\nvec2f_wobble(int a) {\n    return a;\n}\n"
        self.assertTrue(any("vec2f_wobble" in p for p in problems_for(header_extra=extra)))

    def test_a_hand_written_function_the_page_names_passes(self):
        extra = "static inline int\nvec2f_wobble(int a) {\n    return a;\n}\n"
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root, header_extra=extra, page_extra="`vec2f_wobble(a)`\n")
            with (root / "docs/math/Linear.md").open("a") as page:
                page.write("`vec2f_wobble(a)`\n")
            self.assertEqual([], check_package_docs.check(root))

    def test_a_page_entry_defined_in_a_scalar_header_passes(self):
        definition = "static inline int\nmathi_wrap(int a) {\n    return a;\n}\n"
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root, scalar_extra=definition, page_extra="`mathi_wrap(a)`\n")
            (root / "docs/math/Scalar.md").write_text("`mathi_wrap(a)`\n")
            self.assertEqual([], check_package_docs.check(root))

    def test_a_page_entry_removed_from_a_scalar_header_fails(self):
        problems = problems_for(page_extra="`mathi_wrap(a)`\n")
        self.assertTrue(any("mathi_wrap(" in p for p in problems), problems)

    def test_an_identity_initializer_the_page_lacks_fails(self):
        extra = "#define TRANSFORMQ_IDENTITY {0}\n"
        self.assertTrue(any("TRANSFORMQ_IDENTITY" in p for p in problems_for(header_extra=extra)))

    def test_an_instantiation_the_page_does_not_name_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root)
            (root / "launcher/packages/math/include/math/linear/vec2q.h").write_text("MATH_DEFINE_VEC2(vec2q, float)\n")
            self.assertTrue(any("vec2q" in p for p in check_package_docs.check(d)))

    def test_a_missing_family_section_fails(self):
        self.assertTrue(any("no family section for `quat`" in p for p in problems_for(skip_section="quat")))

    def test_a_template_that_defines_nothing_fails_instead_of_passing_on_nothing(self):
        self.assertTrue(any("defines no functions" in p for p in problems_for(empty_template="mat4")))

    def test_a_page_entry_for_a_function_that_no_longer_exists_fails(self):
        self.assertTrue(any("vec2f_gone(" in p for p in problems_for(page_extra="`vec2f_gone(a)`\n")))

    def test_a_page_entry_for_an_existing_function_passes(self):
        self.assertEqual([], problems_for(page_extra="`vec2f_add(a, b)` and `P_add(a)`\n"))

    def test_code_fences_are_not_read_as_entries(self):
        self.assertEqual([], problems_for(page_extra="```c\nvec2f_gone(1);\n```\n"))


SWIZZLE_TEMPLATE = (
    "#define MATH_DEFINE_SWIZZLE2(D, S, N) MATH_SWIZZLE_EACH2(N, MATH_SWIZZLE2, D, S)\n"
    "#define MATH_DEFINE_VEC_SWIZZLE(P, V, T) \\\n"
    "    MATH_DEFINE_SWIZZLE2(V, P, 3) \\\n"
    "    static inline P##_t P##_from_xy(V##_t xy, T z) { return (P##_t){xy.x, xy.y, z}; }\n"
)
COMPLETE = {"vec2": "| `P_<c1><c2>(v)` |\n", "vec3": "| `P_<c1><c2>(v)` | `P_from_xy(xy, z)` |\n"}


def swizzle_problems(sections=None, page_extra="", vec2_n=2):
    """vec2f with its own swizzles; vec3f with swizzles onto vec2f, and from_xy."""
    sections = COMPLETE if sections is None else sections
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        make_tree(root)
        math = root / "launcher/packages/math/include/math/linear"
        (math / "vec_swizzle_template.h").write_text(SWIZZLE_TEMPLATE)
        for family, fields, call in (
            ("vec2", "x, y", "    MATH_DEFINE_SWIZZLE2(P, P, %d)\n" % vec2_n),
            ("vec3", "x, y, z", ""),
        ):
            (math / (family + "_template.h")).write_text(
                "#define MATH_DEFINE_%s(P, T) \\\n    typedef struct { \\\n        T %s; \\\n    } P##_t; \\\n"
                % (family.upper(), fields)
                + FUNCTION.rstrip("\n")
                + (" \\\n" + call if call else "\n")
            )
        (math / "vec3f.h").write_text("MATH_DEFINE_VEC3(vec3f, float)\nMATH_DEFINE_VEC_SWIZZLE(vec3f, vec2f, float)\n")
        page = "".join(
            "### %s\n\n| `P_add(` |\n%s\n" % (f, sections.get(f, "")) for f in check_package_docs.FAMILIES
        )
        write_pages(root, page + "`vec2f` `vec3f`\n" + page_extra)
        return check_package_docs.check(d)


class Swizzles(unittest.TestCase):
    """A swizzle family is read from the templates: its length from the
    MATH_DEFINE_SWIZZLE<k> call, its letters from the source's struct."""

    def test_a_page_with_every_swizzle_pattern_passes(self):
        self.assertEqual([], swizzle_problems())

    def test_a_swizzle_pattern_missing_from_its_source_section_fails(self):
        problems = swizzle_problems(sections={"vec3": COMPLETE["vec3"]})
        self.assertEqual(["docs/math/Linear.md: the vec2 section lacks `P_<c1><c2>(`"], problems)

    def test_a_cross_template_function_belongs_to_its_first_argument_family(self):
        problems = swizzle_problems(sections={"vec2": COMPLETE["vec2"], "vec3": "| `P_<c1><c2>(v)` |\n"})
        self.assertEqual(["docs/math/Linear.md: the vec3 section lacks `P_from_xy(`"], problems)

    def test_only_the_swizzles_the_templates_make_may_be_named(self):
        self.assertEqual([], swizzle_problems(page_extra="`vec3f_zx(v)` `vec2f_yy(v)` `vec3f_from_xy(xy, z)`\n"))
        rows = (("a letter the source lacks", "vec2f_yz("), ("a length no call makes", "vec2f_xyx("))
        for why, name in rows:
            problems = swizzle_problems(page_extra="`%sv)`\n" % name)
            self.assertTrue(any(name in p for p in problems), why)

    def test_a_function_is_valid_only_under_a_prefix_of_its_own_family(self):
        self.assertEqual([], swizzle_problems(page_extra="`vec3f_from_xy(xy, z)` `P_from_xy(xy, z)`\n"))
        problems = swizzle_problems(page_extra="`vec2f_from_xy(xy, z)`\n")
        self.assertTrue(any("vec2f_from_xy(" in p for p in problems), problems)

    def test_a_component_count_that_disagrees_with_the_struct_fails(self):
        self.assertTrue(any("vec2 has 2" in p for p in swizzle_problems(vec2_n=3)))


SPRING_HEADER = "typedef struct {\n    int count;\n} spring_t;\nvoid spring_step(spring_t* s);\n"


def package_problems(files):
    """The per-package rule's problems over a tree of `files`, {path: text}."""
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        for path, text in files.items():
            (root / path).parent.mkdir(parents=True, exist_ok=True)
            (root / path).write_text(text, encoding="utf-8")
        return check_package_docs.package_problems(root)


class PackageDocs(unittest.TestCase):
    TREE = {
        "launcher/packages/physics/include/physics/spring/spring.h": SPRING_HEADER,
        "docs/physics/README.md": "[Spring](Spring.md)\n",
        "docs/physics/Spring.md": "`spring_t` and `spring_step`\n",
    }

    def test_a_package_whose_pages_name_every_header_passes(self):
        self.assertEqual([], package_problems(self.TREE))

    def test_each_broken_rule_is_named(self):
        rows = (
            ("a name missing from its folder's page", {"docs/physics/Spring.md": "`spring_t`\n"},
             "docs/physics/Spring.md: the page does not name spring_step (spring.h)"),
            ("a type missing from its folder's page", {"docs/physics/Spring.md": "`spring_step`\n"},
             "docs/physics/Spring.md: the page does not name spring_t (spring.h)"),
            ("a folder with no page", {"launcher/packages/physics/include/physics/fluid/flip.h": "#pragma once\n"},
             "docs/physics/README.md: fluid/ has no page Fluid.md"),
            ("a page the index does not link", {"docs/physics/Fluid.md": "nothing\n"},
             "docs/physics/README.md: index lacks a link to Fluid.md"),
            ("a header outside a folder", {"launcher/packages/physics/include/physics/loose.h": "#pragma once\n"},
             "launcher/packages/physics/include/physics/loose.h sits in no folder, so it has no page"),
            ("an overlong formula", {"docs/physics/Spring.md": "`spring_t` `spring_step` $" + "x" * 111 + "$\n"},
             "docs/physics/Spring.md:1: inline math has 111 characters"),
        )
        for why, change, expected in rows:
            with self.subTest(why=why):
                problems = package_problems({**self.TREE, **change})
                self.assertTrue(any(p.startswith(expected) for p in problems), problems)

    def test_declarations_with_general_return_types_are_required(self):
        header = "launcher/packages/physics/include/physics/spring/spring.h"
        for result in ("bool", "int", "uint32_t", "const unsigned long", "const char*", "void* const"):
            with self.subTest(result=result):
                problems = package_problems({**self.TREE, header: SPRING_HEADER + result + " spring_ready(void);\n"})
                self.assertTrue(any("spring_ready" in p for p in problems), problems)

    def test_a_real_package_requires_an_undocumented_declaration(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            for directory in ("launcher/packages/kinematics", "docs/kinematics"):
                shutil.copytree(REPO / directory, root / directory)
            self.assertEqual([], check_package_docs.package_problems(root))
            header = root / "launcher/packages/kinematics/include/kinematics/orbit/orbit_motion.h"
            header.write_text(header.read_text() + "\nbool orbit_undocumented(void);\n")
            self.assertTrue(any("orbit_undocumented" in p for p in check_package_docs.package_problems(root)))

    def test_a_docs_folder_without_a_package_and_a_package_without_docs_are_not_checked(self):
        self.assertEqual([], package_problems({
            "docs/firmware/README.md": "no package\n",
            "launcher/packages/name_hash/include/name_hash/name_hash.h": SPRING_HEADER,
        }))


if __name__ == "__main__":
    unittest.main()
