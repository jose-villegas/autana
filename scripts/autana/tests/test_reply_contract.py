"""Every reply autana waits for is one the firmware prints and the host's
reply filter keeps: read from autana.py's send() calls and the firmware's
own string literals, so a new verb or a changed reply is covered unasked."""
import ast
import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts" / "lib"))
sys.path.insert(0, str(REPO / "scripts" / "gates"))
from c_lexer import tokens  # noqa: E402
from console_protocol import FRAMED_ENDINGS, replies_to  # noqa: E402

AUTANA = REPO / "scripts" / "autana" / "autana.py"
FIRMWARE = REPO / "launcher" / "main"
PRINTF_FIELD = re.compile(r"%[-+ #0-9.*]*[hljzt]*[a-zA-Z%]")


def literal(node):
    try:
        return ast.literal_eval(node)
    except ValueError:
        return None


def send_contracts():
    """(reply, until) of each send() call whose reply is spelled out, with
    send()'s own defaults; a reply built at run time (a verb's own name) is
    left to that verb's tests."""
    defaults = {}
    calls = []
    for node in ast.walk(ast.parse(AUTANA.read_text(encoding="utf-8"))):
        if isinstance(node, ast.FunctionDef) and node.name == "send":
            names = [arg.arg for arg in node.args.args][-len(node.args.defaults):]
            defaults = {name: literal(value) for name, value in zip(names, node.args.defaults)}
        elif isinstance(node, ast.Call) and getattr(node.func, "id", None) == "send":
            calls.append({kw.arg: literal(kw.value) for kw in node.keywords if kw.arg in ("reply", "until")})
    contracts = []
    for call in calls:
        reply = call.get("reply", defaults["reply"])
        if reply is not None and call.get("until", ()) is not None:
            contracts.append((reply, call.get("until") or [reply]))
    return contracts


def firmware_lines():
    """Each firmware string literal as the line it prints, fields filled in."""
    lines = set()
    for path in list(FIRMWARE.rglob("*.c")) + list(FIRMWARE.rglob("*.h")):
        source = path.read_text(encoding="utf-8", errors="replace")
        for kind, start, end in tokens(source):
            if kind != "literal" or source[start] != '"':
                continue
            line = PRINTF_FIELD.sub("0", source[start + 1:end - 1].replace("\\n", "\n")).strip()
            if line:
                lines.add(line)
    return lines


class ReplyContractTests(unittest.TestCase):
    def test_every_awaited_reply_is_printed_and_kept(self):
        contracts = send_contracts()
        self.assertGreater(len(contracts), 1, "no send() calls read from autana.py")
        lines = firmware_lines()
        for reply, until in contracts:
            with self.subTest(reply=reply):
                # A terminator alone (HEAP_END) is not the reply a caller waits for.
                kept = [line for line in lines if replies_to(line.encode() + b"\n", reply, ())[0]
                        and not line.startswith(tuple(reply.rstrip("_") + ending for ending in FRAMED_ENDINGS))]
                printed = [line for line in lines if line.startswith(reply)]
                self.assertTrue(kept, f"no firmware line {printed} survives the {reply!r} reply filter")
            for ending in until:
                with self.subTest(reply=reply, until=ending):
                    self.assertTrue(any(line.startswith(ending) and replies_to(line.encode() + b"\n", reply, until)[1]
                                        for line in lines),
                                    f"no firmware line starts with {ending!r} and ends the {reply!r} reply")


if __name__ == "__main__":
    unittest.main()
