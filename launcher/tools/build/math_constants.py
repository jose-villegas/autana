"""Named aggregate math constants discovered from their defining headers.

Standard library only: bake keys and the device tool reach this through c_header, so it must not pull in
the gates (their requirements would join every mesh key, and their import-time scans read source)."""
import ast
import functools
import operator
import pathlib
import re


MATH_HEADERS = "launcher/packages/math/include/math"
# A string or char literal is kept, so a comment marker inside one is not taken for a comment.
COMMENT = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', re.S)
INTEGER_DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+([^\n]+)$", re.M)
OPERATORS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
             ast.LShift: operator.lshift, ast.RShift: operator.rshift}


def blank_comments(text):
    """`text` with every comment blanked to spaces, newlines kept so offsets and lines hold."""
    return COMMENT.sub(lambda m: m[0] if m[0][0] in "\"'" else re.sub(r"[^\n]", " ", m[0]), text)


def macro_calls(text):
    """(callee, argument text) of each MATH_* call in `text`, nested parentheses included."""
    text = re.sub(r"\\\r?\n", "", text)
    for match in re.finditer(r"\b(MATH_\w+)\(", text):
        depth = 0
        for end in range(match.end() - 1, len(text)):
            depth += (text[end] == "(") - (text[end] == ")")
            if depth == 0:
                yield match[1], text[match.end():end]
                break


def split_args(text):
    return [a.strip() for a in text.split(",")]


def integer_defines(sources):
    """Every #define in `sources` whose body folds to an integer, through the other defines it names."""
    expressions = {m[1]: re.sub(r"[uUlL]+\b", "", m[2]).strip()
                   for code in sources for m in INTEGER_DEFINE.finditer(code)}
    values = {}

    def fold(node, seen):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.Name) and node.id in expressions and node.id not in seen:
            return value(node.id, seen | {node.id})
        if isinstance(node, ast.BinOp) and type(node.op) in OPERATORS:
            return OPERATORS[type(node.op)](fold(node.left, seen), fold(node.right, seen))
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            return -fold(node.operand, seen)
        raise ValueError(node)

    def value(name, seen):
        if name not in values:
            values[name] = fold(ast.parse(expressions[name], mode="eval").body, seen)
        return values[name]

    for name in expressions:
        try:
            value(name, {name})
        except (SyntaxError, ValueError, TypeError):
            values.pop(name, None)
    return values


def math_constant_table(root):
    headers = pathlib.Path(root) / MATH_HEADERS
    signature = tuple((str(path), path.stat().st_mtime_ns, path.stat().st_size)
                      for path in sorted(headers.rglob("*.h")))
    return _math_constant_table(signature)


@functools.lru_cache(maxsize=8)
def _math_constant_table(signature):
    sources = {path: blank_comments(pathlib.Path(path).read_text(encoding="utf-8"))
               for path, _, _ in signature}
    values = integer_defines(sources.values())
    templates, macros = {}, {}
    for path, code in sources.items():
        for macro in INTEGER_DEFINE.finditer(re.sub(r"\\\r?\n", "", code)):
            if macro[2].strip().startswith("{"):
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
