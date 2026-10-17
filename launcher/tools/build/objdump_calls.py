"""Who calls whom, read from the Xtensa objdump's disassembly."""

import re

FUNCTION = re.compile(r"^([0-9a-f]+) <([^>]+)>:\s*$")


def function_addresses(text):
    """{function: address} for every function header in `objdump -d` text."""
    found = {}
    for line in text.splitlines():
        m = FUNCTION.match(line)
        if m:
            found.setdefault(m.group(2), int(m.group(1), 16))
    return found


def parse_calls(text, call):
    """{function: set of called symbols} from `objdump -d` text, where `call`
    matches a line naming a callee in its group 1."""
    calls = {}
    current = None
    for line in text.splitlines():
        m = FUNCTION.match(line)
        if m:
            # The literal pool a function loads its calls from is a section of
            # its own, named for the function: they are one function here.
            current = calls.setdefault(m.group(2).removeprefix(".literal."), set())
            continue
        m = call.search(line)
        if m and current is not None:
            current.add(re.sub(r"[+-]0x[0-9a-f]+$", "", m.group(1)))
    return calls
