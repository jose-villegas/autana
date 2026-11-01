"""Named aggregate math constants discovered from their defining headers."""
import functools
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "scripts/gates"))
from c_comments import blank_comments  # noqa: E402
from c_constants import DEFINE, definitions, fold  # noqa: E402
from check_package_docs import macro_calls, split_args  # noqa: E402


MATH_HEADERS = "launcher/packages/math/include/math"


def math_constant_table(root):
    headers = pathlib.Path(root) / MATH_HEADERS
    signature = tuple((str(path), path.stat().st_mtime_ns, path.stat().st_size)
                      for path in sorted(headers.rglob("*.h")))
    return _math_constant_table(signature)


@functools.lru_cache(maxsize=8)
def _math_constant_table(signature):
    sources = {path: blank_comments(pathlib.Path(path).read_text(encoding="utf-8"), "code")
               for path, _, _ in signature}
    values = {}
    pending = [d for path, code in sources.items() for d in definitions(str(path), code)
               if d.kind == "define"]
    for _ in range(len(pending)):
        before = len(values)
        for definition in pending:
            value = fold(definition.expression, values)
            if value is not None:
                values[definition.name] = value
        if len(values) == before:
            break
    templates, macros = {}, {}
    for path, code in sources.items():
        for macro in DEFINE.finditer(code):
            body = re.sub(r"\\\r?\n", "", macro[2]).strip()
            if body.startswith("{"):
                macros[macro[1]] = macro[1].lower() + "()"
        code = re.sub(r"\\\r?\n", "", code)
        for definition in re.finditer(r"#define\s+(MATH_DEFINE_\w+)\(([^)]*)\)(.*?)(?=#define|\Z)", code, re.S):
            constants = {}
            fields = re.search(r"typedef\s+struct\s*\{\s*T\s+([\w,\s]+);\s*\}", definition[3])
            if not fields:
                continue
            components = tuple(re.findall(r"\w+", fields[1]))
            scalar = {"OPS##_zero()": 0, "OPS##_one()": 1, "OPS##_neg(OPS##_one())": -1}
            aliases = []
            for callee, arguments in macro_calls(definition[3]):
                arguments = split_args(re.sub(r"\s+", "", arguments))
                if callee == "MATH_DEFINE_CONSTANT_ALIAS" and len(arguments) == 3 and arguments[0] == "P":
                    aliases.append((arguments[1], arguments[2]))
                if callee != "MATH_DEFINE_CONSTANT":
                    continue
                if len(arguments) != len(components) + 2 or arguments[0] != "P":
                    continue
                if all(entry in scalar for entry in arguments[2:]):
                    constants[arguments[1]] = tuple(scalar[entry] for entry in arguments[2:])
            for _ in aliases:
                for name, target in aliases:
                    if target in constants:
                        constants[name] = constants[target]
            templates[definition[1]] = (components, constants)
    table, calls = {}, {}
    for code in sources.values():
        for instance in re.finditer(r"\b(MATH_DEFINE_\w+)\(([^()]*)\)", code):
            if instance[1] not in templates:
                continue
            arguments = [arg.strip() for arg in instance[2].split(",")]
            prefix = arguments[0]
            if prefix == "P":
                continue
            components, constants = templates[instance[1]]
            unit = values["MATHX_ONE"] if "mathx" in arguments else 1
            entries = {}
            for name, value in constants.items():
                calls[prefix + "_" + name + "()"] = tuple(v * unit for v in value)
                entries.setdefault(tuple(v * unit for v in value), prefix + "_" + name + "()")
            table[prefix + "_t"] = (components, entries)
    return table, macros, values, calls
