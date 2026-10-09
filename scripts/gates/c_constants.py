"""Shared integer constant index for source and documentation gates."""
import ast
from collections import Counter, defaultdict
from dataclasses import dataclass
import operator
import pathlib
import re

from c_comments import blank_comments
from tracked import tracked_files

DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)\b([^\n]*)", re.M)
ENUM = re.compile(r"\benum\s*(?:[A-Za-z_]\w*\s*)?\{([^{}]*)\}", re.S)
MEMBER = re.compile(r"^\s*([A-Za-z_]\w*)\s*=\s*(.+?)\s*$", re.S)
LITERAL = re.compile(r"(?:0[xX][0-9a-fA-F]+|0|[1-9]\d*)[uUlL]*$")
INTEGER = re.compile(r"(?<![\w.])(?:0[xX][0-9a-fA-F]+|0|[1-9]\d*)[uUlL]*(?![\w.])")
SCALAR = re.compile(r"\b(?:static\s+)?const\s+(?:[A-Za-z_]\w*\s+)+([A-Za-z_]\w*)\s*=\s*([^;{},]+);", re.S)
OPERATORS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
             ast.LShift: operator.lshift, ast.RShift: operator.rshift,
             ast.BitOr: operator.or_, ast.BitAnd: operator.and_, ast.BitXor: operator.xor}


def uncomment(text):
    return blank_comments(text)


def source_files(root):
    root = pathlib.Path(root)
    for name in sorted(tracked_files(root, ("launcher",))):
        path = root / name
        if (path.suffix in {".c", ".h"} and path.is_file()
                and "managed_components" not in path.parts
                and not any(part.startswith("build") for part in path.relative_to(root).parts)):
            yield path


@dataclass(frozen=True)
class Definition:
    name: str
    expression: str
    path: str
    line: int
    kind: str


def definitions(path, text):
    code = blank_comments(text, "code")
    for match in DEFINE.finditer(code):
        expression = match[2].strip()
        if match[2].startswith("("):
            expression = ""
        yield Definition(match[1], expression, path, code.count("\n", 0, match.start(1)) + 1, "define")
    for enum in ENUM.finditer(code):
        offset = enum.start(1)
        for field in enum[1].split(","):
            member = MEMBER.match(field)
            if member:
                name, expression = member.groups()
            elif field.strip():
                name, expression = field.strip().split()[0], ""
            else:
                offset += len(field) + 1
                continue
            name_offset = offset + field.find(name)
            yield Definition(name, expression, path, code.count("\n", 0, name_offset) + 1, "enum")
            offset += len(field) + 1
    for match in SCALAR.finditer(code):
        yield Definition(match[1], match[2], path, code.count("\n", 0, match.start(1)) + 1, "scalar")


def fold(expression, values):
    expression = INTEGER.sub(lambda match: re.sub(r"[uUlL]+$", "", match[0]), expression)

    def evaluate(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.Name):
            return values[node.id]
        if isinstance(node, ast.BinOp) and type(node.op) in OPERATORS:
            return OPERATORS[type(node.op)](evaluate(node.left), evaluate(node.right))
        if isinstance(node, ast.UnaryOp):
            operand = evaluate(node.operand)
            if isinstance(node.op, ast.USub):
                return -operand
            if isinstance(node.op, ast.UAdd):
                return operand
            if isinstance(node.op, ast.Invert):
                return ~operand
        raise ValueError("not an integer expression")

    try:
        return evaluate(ast.parse(expression.strip(), mode="eval").body)
    except (SyntaxError, ValueError, KeyError, TypeError, OverflowError):
        return None


class ConstantIndex:
    def __init__(self, texts):
        self.by_file = {path: tuple(definitions(path, text)) for path, text in texts.items()}

    def visible(self, paths):
        records = [record for path in sorted(paths) for record in self.by_file.get(path, ())]
        counts = Counter(record.name for record in records)
        values = {}
        pending = records.copy()
        resolved = []
        while pending:
            remaining = []
            newly_resolved = []
            for record in pending:
                value = fold(record.expression, values)
                if value is None:
                    remaining.append(record)
                else:
                    newly_resolved.append((record, value))
            if len(remaining) == len(pending):
                break
            resolved.extend(newly_resolved)
            by_name = defaultdict(list)
            for record, value in resolved:
                by_name[record.name].append(value)
            values = {name: candidates[0] for name, candidates in by_name.items()
                      if len(candidates) == counts[name] and len(set(candidates)) == 1}
            pending = remaining
        return resolved


def constants(root, *, literal_only=False):
    texts = {path.relative_to(root).as_posix(): path.read_text(encoding="utf-8", errors="replace")
             for path in source_files(root)}
    index = ConstantIndex(texts)
    if literal_only:
        # Prose claims use the documentation gate's decimal define/enum vocabulary.
        index.by_file = {path: tuple(record for record in records if record.kind != "scalar")
                         for path, records in index.by_file.items()}
    counts = {}
    for records in index.by_file.values():
        for record in records:
            counts[record.name] = counts.get(record.name, 0) + 1
    return {record.name: value for record, value in index.visible(texts)
            if counts[record.name] == 1 and (not literal_only or
               LITERAL.fullmatch(record.expression) and not record.expression.lower().startswith("0x"))}
