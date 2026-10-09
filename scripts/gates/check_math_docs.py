#!/usr/bin/env python3
"""Fail when docs/math/README.md and math/linear/ disagree about what exists.

    python scripts/gates/check_math_docs.py [--root ROOT]

The reference page names every function of every family and every
instantiated type; the headers are the truth, read here and never listed by
hand:

  * a family's functions are the `static inline ... P##_name(` definitions in
    its `<family>_template.h`; the page's section for that family must
    contain `P_name(`;
  * an instantiation is a `MATH_DEFINE_<FAMILY>(prefix, ...)` call in a
    header; the page must name `prefix` in backticks;
  * a template outside the five families (vec_swizzle_template.h) adds its
    `P##_name(` functions to the family of the prefix its call passes as P;
  * a swizzle family is a `MATH_DEFINE_SWIZZLE<k>(D, S, N)` call in a
    template macro: S's family section must contain the pattern
    `P_<c1>...<ck>(`, its names are every k letters of S's struct fields,
    and N must be that field count;
  * every function an instantiation header or vec_convert.h writes by hand
    (`quatf_slerp`, `vec3f_from_vec3x`, `mathf_to_x`, ...) and each
    `TRANSFORM<S>_IDENTITY` initializer must be named on the page;
  * the other way round: every `name(` the page puts in backticks must exist,
    as `P_` or a prefix of that function's own family, or as a function a
    header defines.
"""

import argparse
import itertools
import pathlib
import re
import sys

MATH = pathlib.Path("launcher/main/math/linear")
PAGE = pathlib.Path("docs/math/README.md")

FAMILIES = ("vec2", "vec3", "quat", "mat4", "transform")
TEMPLATE_FUNCTION = re.compile(r"^\s*static inline [^\n(]*?P##_(\w+)\(", re.M)
DEFINE_CALL = re.compile(r"^MATH_DEFINE_\w+\((\w+),", re.M)
HEADER_CALL = re.compile(r"^(MATH_DEFINE_\w+)\(([^)]*)\)", re.M)
TEMPLATE_MACRO = re.compile(r"^#define (MATH_DEFINE_\w+)\(([^)]*)\)(.*?)(?=^#define |\Z)", re.M | re.S)
SWIZZLE_CALL = re.compile(r"\bMATH_DEFINE_SWIZZLE(\d+)\((\w+), (\w+), (\d+)\)")
FIELDS = re.compile(r"typedef struct \{[\s\\]*T ([\w, ]+);")
HAND_WRITTEN = re.compile(r"^static inline [^\n]*\n(\w+)\(", re.M)
IDENTITY = re.compile(r"^#define (TRANSFORM\w+_IDENTITY)\b", re.M)
FENCE = re.compile(r"^```.*?^```", re.M | re.S)
PAGE_CALL = re.compile(r"`[^`\n]*?\b([A-Za-z][A-Za-z0-9]*_[a-z0-9_]+)\(")


def section(text, family):
    """The page's `### <family>` section, up to the next heading."""
    m = re.search(r"^### %s\b.*?(?=^#{1,3} |\Z)" % re.escape(family), text, re.M | re.S)
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
    family_templates = {f + "_template.h" for f in FAMILIES}
    macros = {}
    fields = {}
    for template in sorted((root / MATH).glob("*_template.h")):
        text = read(template)
        for name, params, body in TEMPLATE_MACRO.findall(text):
            macros[name] = (split_args(params), body, template.name in family_templates)
        m = FIELDS.search(text)
        if template.name in family_templates and m:
            fields[template.name[: -len("_template.h")]] = split_args(m.group(1))

    calls = []
    for header in sorted((root / MATH).glob("*.h")):
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
    for name, args in calls:
        if name not in macros or args[0] not in family_of:
            continue
        params, body, own_family = macros[name]
        if not own_family:
            functions[family_of[args[0]]].update(TEMPLATE_FUNCTION.findall(body))
        bind = dict(zip(params, args))
        for length, _, source, count in SWIZZLE_CALL.findall(body):
            prefix = bind.get(source, source)
            family = family_of.get(prefix)
            letters = fields.get(family, [])
            if int(count) != len(letters):
                problems.append(
                    "%s(%s) swizzles %s as %s components, but %s has %d"
                    % (name, args[0], prefix, count, family, len(letters))
                )
                continue
            patterns[family].add("P_%s(" % "".join("<c%d>" % i for i in range(1, int(length) + 1)))
            names.update(prefix + "_" + "".join(t) for t in itertools.product(letters, repeat=int(length)))
    return family_of, functions, patterns, names, problems


def check(root):
    root = pathlib.Path(root)
    page = read(root / PAGE)
    family_of, functions, patterns, swizzle_names, problems = instantiations(root)

    for family in FAMILIES:
        template = root / MATH / (family + "_template.h")
        own = set(TEMPLATE_FUNCTION.findall(read(template)))
        if not own:
            problems.append("%s defines no functions: the gate would pass on nothing" % template.name)
        functions[family] |= own
        body = section(page, family)
        if not body:
            problems.append("%s has no `### %s` section" % (PAGE.as_posix(), family))
            continue
        wanted = ["P_%s(" % name for name in sorted(functions[family])] + sorted(patterns[family])
        for entry in wanted:
            if entry not in body:
                problems.append("%s: the %s section lacks `%s`" % (PAGE.as_posix(), family, entry))
    family_functions = set().union(*functions.values())

    defined = set()
    for header in sorted((root / MATH).glob("*.h")):
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
