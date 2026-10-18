#!/usr/bin/env python3
"""Fail when docs/math/*.md and the math headers disagree about what exists.

    python scripts/gates/check_math_docs.py [--root ROOT]

Each family's folder names its reference page (linear/ becomes Linear.md).
The index links every page. Motion headers' public types and functions must
be named in Motion.md. The reference pages name every family function and
instantiated type; the headers are the truth, read here and never listed by
hand:

  * a family's functions are the `static inline ... P##_name(` definitions in
    its `<family>_template.h`; the page's section for that family must
    contain `P_name(`;
  * an instantiation is a `MATH_DEFINE_<FAMILY>(prefix, ...)` call in a
    header; the page must name `prefix` in backticks;
  * template macro calls bind arguments recursively, so shared definers add
    their `P##_name(` and `P##_##OP(` functions to the calling family;
  * a swizzle family is a `MATH_DEFINE_SWIZZLE<k>(D, S, N)` call in a
    template macro: S's family section must contain the pattern
    `P_<c1>...<ck>(`, its names are every k letters of S's struct fields,
    and N must be that field count;
  * every function an instantiation header or vec_convert.h writes by hand
    (`quatf_slerp`, `vec3f_from_vec3x`, `mathf_to_x`, ...) and each
    `TRANSFORM<S>_IDENTITY` initializer must be named on the page;
  * the other way round: every `name(` the page puts in backticks must exist,
    as `P_` or a prefix of that function's own family, or as a function a
    header defines;
  * every inline formula, in $...$ or $`...`$, has at most
    MAX_INLINE_MATH_CHARS characters of TeX outside code spans and fences;
    a longer one is split into one span per step, joined with <br>.
    Fenced blocks and `$$...$$` display math are not measured.
"""

import argparse
import itertools
import pathlib
import re
import sys

MATH = pathlib.Path("launcher/packages/math/include/math/linear")
PAGE = pathlib.Path("docs/math/README.md")

# An inline formula cannot wrap, so a long one overflows a phone-width column.
# TeX source length stands in for rendered width: 110 fits one step, not a derivation.
MAX_INLINE_MATH_CHARS = 110

def family_dirs(root):
    """Primary type templates, keyed by family and owned by their folder."""
    directories = {}
    for template in sorted((root / MATH.parent).rglob("*_template.h")):
        family = template.stem.removesuffix("_template")
        text = template.read_text(encoding="utf-8")
        if (re.search(r"^#define MATH_DEFINE_" + family.upper() + r"\(", text, re.M)
                and re.search(r"typedef struct", text)):
            directories[family] = template.parent.relative_to(root)
    return directories



FAMILY_DIRS = family_dirs(pathlib.Path(__file__).resolve().parents[2])
FAMILIES = tuple(FAMILY_DIRS)


def family_page(directory):
    return PAGE.parent / (directory.name.capitalize() + ".md")


TEMPLATE_FUNCTION = re.compile(r"^\s*static inline (?:__attribute__\(\(\w+\)\) )?[^\n(]*?P##_(\w+)\(", re.M)
DEFINE_CALL = re.compile(r"^MATH_DEFINE_\w+\((\w+),", re.M)
HEADER_CALL = re.compile(r"^(MATH_DEFINE_\w+)\(([^)]*)\)", re.M)
TEMPLATE_MACRO = re.compile(r"^#define (MATH_\w+)\(([^)]*)\)(.*?)(?=^#define |\Z)", re.M | re.S)
MACRO_CALL = re.compile(r"\b(MATH_\w+)\(([^()]*)\)")
BOUND_FUNCTION = re.compile(r"^\s*static inline (?:__attribute__\(\(\w+\)\) )?[^\n(]*?P##_(?:##(\w+)|(\w+))\(", re.M)
SWIZZLE_CALL = re.compile(r"\bMATH_DEFINE_SWIZZLE(\d+)\((\w+), (\w+), (\d+)\)")
FIELDS = re.compile(r"typedef struct \{[\s\\]*T ([\w, ]+);")
HAND_WRITTEN = re.compile(r"^(?:static inline [^\n]*\n|(?:void|float|[A-Za-z]\w*_t) )([A-Za-z]\w*)\(", re.M)
PUBLIC_TYPE = re.compile(r"^typedef struct\s*\{.*?\}\s*(\w+);", re.M | re.S)
IDENTITY = re.compile(r"^#define (TRANSFORM\w+_IDENTITY)\b", re.M)
FENCE = re.compile(r"^```.*?^```", re.M | re.S)
INLINE_MATH = re.compile(r"\$`([^`\n]*)`\$|(`+).*?(?<!`)\2(?!`)|(?<![\\$])\$(?!\$)([^$\n]*)\$(?!\$)")
PAGE_CALL = re.compile(r"`[^`\n]*?\b([A-Za-z][A-Za-z0-9]*_[a-z0-9_]+)\(")


def section(text, family):
    """The page's `## <family>` or `### <family>` section, up to the next heading."""
    m = re.search(r"^#{2,3} %s\b.*?(?=^#{1,3} |\Z)" % re.escape(family), text, re.M | re.S)
    return m.group(0) if m else ""


def read(path):
    return path.read_text(encoding="utf-8")


def split_args(text):
    return [a.strip() for a in text.split(",")]


def instantiations(root):
    """What the headers' calls of template macros add beyond each family's own
    template: each prefix's family; the `P##_name(` functions of a template
    outside the families, by family; the swizzle pattern each family section
    must name; every swizzle name; and problems."""
    FAMILY_DIRS = family_dirs(root)
    FAMILIES = tuple(FAMILY_DIRS)
    family_templates = {f + "_template.h" for f in FAMILIES}
    macros = {}
    fields = {}
    for template in sorted(path for directory in set(FAMILY_DIRS.values())
                           for path in (root / directory).glob("*_template.h")):
        text = read(template)
        for name, params, body in TEMPLATE_MACRO.findall(text):
            macros[name] = (split_args(params), body)
        m = FIELDS.search(text)
        if template.name in family_templates and m:
            fields[template.name[: -len("_template.h")]] = split_args(m.group(1))

    calls = []
    for header in sorted(path for directory in set(FAMILY_DIRS.values())
                         for path in (root / directory).glob("*.h")):
        if not header.name.endswith("_template.h"):
            calls += [(name, split_args(args)) for name, args in HEADER_CALL.findall(read(header))]
    family_of = {}
    for name, args in calls:
        family = name[len("MATH_DEFINE_") :].lower()
        if family in FAMILIES:
            family_of[args[0]] = family

    functions = {f: set() for f in FAMILIES}
    patterns = {f: set() for f in FAMILIES}
    names = set()
    problems = []

    def visit(name, args, family, active):
        if name not in macros or name in active:
            return
        params, body = macros[name]
        bind = dict(zip(params, args))
        for parameter, literal in BOUND_FUNCTION.findall(body):
            functions[family].add(bind.get(parameter, parameter) if parameter else literal)
        for length, _, source, count in SWIZZLE_CALL.findall(body):
            prefix = bind.get(source, source)
            source_family = family_of.get(prefix)
            letters = fields.get(source_family, [])
            if int(count) != len(letters):
                problems.append(
                    "%s(%s) swizzles %s as %s components, but %s has %d"
                    % (name, args[0], prefix, count, source_family, len(letters))
                )
                continue
            patterns[source_family].add("P_%s(" % "".join("<c%d>" % i for i in range(1, int(length) + 1)))
            names.update(prefix + "_" + "".join(t) for t in itertools.product(letters, repeat=int(length)))
        for callee, arguments in MACRO_CALL.findall(body):
            bound = [bind.get(arg, arg) for arg in split_args(arguments)]
            visit(callee, bound, family, active | {name})

    for name, args in calls:
        if args[0] in family_of:
            visit(name, args, family_of[args[0]], set())
    return family_of, functions, patterns, names, problems


def check(root):
    root = pathlib.Path(root)
    FAMILY_DIRS = family_dirs(root)
    FAMILIES = tuple(FAMILY_DIRS)
    pages = {p.relative_to(root): read(p) for p in sorted((root / PAGE.parent).glob("*.md"))}
    # Benchmarks also cite platform APIs; their formulas and index link are still checked.
    page = "\n".join(text for path, text in pages.items() if path.name != "Benchmark.md")
    family_of, functions, patterns, swizzle_names, problems = instantiations(root)
    for path, text in pages.items():
        if path != PAGE and not re.search(r"\]\(" + re.escape(path.name) + r"(?:#[^)]*)?\)", pages[PAGE]):
            problems.append("%s: index lacks a link to %s" % (PAGE.as_posix(), path.name))
        for line_number, line in enumerate(FENCE.sub(lambda m: "\n" * m.group(0).count("\n"), text).splitlines(), start=1):
            for quoted_formula, code_delimiter, plain_formula in INLINE_MATH.findall(line):
                formula = quoted_formula or plain_formula
                if len(formula) > MAX_INLINE_MATH_CHARS:
                    problems.append(
                        "%s:%d: inline math has %d characters (limit %d); "
                        "split the formula into one span per step joined with <br>"
                        % (path.as_posix(), line_number, len(formula), MAX_INLINE_MATH_CHARS)
                    )

    for family in FAMILIES:
        template = root / FAMILY_DIRS[family] / (family + "_template.h")
        own = set(TEMPLATE_FUNCTION.findall(read(template)))
        if not own:
            problems.append("%s defines no functions: the gate would pass on nothing" % template.name)
        functions[family] |= own
        path = family_page(FAMILY_DIRS[family])
        body = section(pages.get(path, ""), family)
        if not body:
            problems.append("%s has no family section for `%s` (heading level 2 or 3)" % (path.as_posix(), family))
            continue
        wanted = ["P_%s(" % name for name in sorted(functions[family])] + sorted(patterns[family])
        for entry in wanted:
            if entry not in body:
                problems.append("%s: the %s section lacks `%s`" % (path.as_posix(), family, entry))
    family_functions = set().union(*functions.values())

    defined = set()
    for header in sorted(path for directory in set(FAMILY_DIRS.values())
                         for path in (root / directory).glob("*.h")):
        text = read(header)
        if header.name.endswith("_template.h"):
            continue
        defined.update(HAND_WRITTEN.findall(text))
        if header.name.startswith("math"):
            continue
        for prefix in DEFINE_CALL.findall(text):
            if "`%s`" % prefix not in page:
                problems.append("%s: the page does not name the type `%s`" % (PAGE.as_posix(), prefix))
        for name in HAND_WRITTEN.findall(text) + IDENTITY.findall(text):
            if name not in page:
                problems.append("%s: the page does not name %s (%s)" % (PAGE.as_posix(), name, header.name))

    for header in sorted((root / MATH.parent).rglob("*.h")):
        text = read(header)
        defined.update(HAND_WRITTEN.findall(text))
        if header.parent.name == "motion":
            path = family_page(header.parent)
            for name in HAND_WRITTEN.findall(text) + PUBLIC_TYPE.findall(text):
                if name not in pages.get(path, ""):
                    problems.append("%s: the page does not name %s (%s)" % (path.as_posix(), name, header.name))

    valid = set(defined) | swizzle_names | {"P_%s" % f for f in family_functions}
    valid |= {"%s_%s" % (p, f) for p, family in family_of.items() for f in functions[family]}
    for name in sorted(set(PAGE_CALL.findall(FENCE.sub("", page)))):
        if name not in valid:
            problems.append("%s names %s(, which no header defines" % (PAGE.as_posix(), name))
    return problems


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=pathlib.Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    problems = check(args.root)
    for p in problems:
        print(p)
    print("%d math documentation problem(s)" % len(problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
