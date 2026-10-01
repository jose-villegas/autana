#!/usr/bin/env python3
"""Fail when docs/math/README.md and util/math/ disagree about what exists.

    python scripts/gates/check_math_docs.py [--root ROOT]

The reference page names every function of every family and every
instantiated type; the headers are the truth, read here and never listed by
hand:

  * a family's functions are the `P##_name(` definitions in its
    `<family>_template.h`; the page's section for that family must contain
    `P_name(`;
  * an instantiation is a `MATH_DEFINE_<FAMILY>(prefix, ...)` call in a
    header; the page must name `prefix` in backticks;
  * a function an instantiation header or vec_convert.h writes by hand
    (`quatf_slerp`, `vec3f_from_vec3x`, ...) must be named on the page;
  * each `TRANSFORM<S>_IDENTITY` initializer must be named on the page.
"""

import argparse
import pathlib
import re
import sys

MATH = pathlib.Path("launcher/main/util/math")
PAGE = pathlib.Path("docs/math/README.md")

SECTIONS = {"vec2": "vec2", "vec3": "vec3", "quat": "quat", "mat4": "mat4", "transform": "transform"}
TEMPLATE_FUNCTION = re.compile(r"^\s*static inline [^\n(]*?P##_(\w+)\(", re.M)
DEFINE_CALL = re.compile(r"^MATH_DEFINE_\w+\((\w+),", re.M)
HAND_WRITTEN = re.compile(r"^(\w+_(?:slerp|from_vec\d\w))\(", re.M)
IDENTITY = re.compile(r"^#define (TRANSFORM\w+_IDENTITY)\b", re.M)


def section(text, family):
    """The page's `### <family>` section, up to the next heading."""
    m = re.search(r"^### %s\b.*?(?=^#{1,3} |\Z)" % re.escape(family), text, re.M | re.S)
    return m.group(0) if m else ""


def check(root):
    root = pathlib.Path(root)
    page = (root / PAGE).read_text(encoding="utf-8")
    problems = []

    for family in SECTIONS:
        template = root / MATH / (family + "_template.h")
        names = sorted(set(TEMPLATE_FUNCTION.findall(template.read_text(encoding="utf-8"))))
        if not names:
            problems.append("%s defines no functions: the gate would pass on nothing" % template.name)
        body = section(page, family)
        if not body:
            problems.append("%s has no `### %s` section" % (PAGE, family))
            continue
        for name in names:
            if "P_%s(" % name not in body:
                problems.append("%s: the %s section lacks `P_%s(`" % (PAGE, family, name))

    for header in sorted((root / MATH).glob("*.h")):
        text = header.read_text(encoding="utf-8")
        if header.name.endswith("_template.h"):
            continue
        for prefix in DEFINE_CALL.findall(text):
            if "`%s`" % prefix not in page:
                problems.append("%s: the page does not name the type `%s`" % (PAGE, prefix))
        for name in HAND_WRITTEN.findall(text) + IDENTITY.findall(text):
            if name not in page:
                problems.append("%s: the page does not name %s (%s)" % (PAGE, name, header.name))
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
