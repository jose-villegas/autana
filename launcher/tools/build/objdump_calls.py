"""Who calls whom, read from the Xtensa objdump's disassembly."""

import re

FUNCTION = re.compile(r"^([0-9a-f]+) <([^>]+)>:\s*$")

CALL_SYMBOL = re.compile(r"[\w.$@]+")
DIRECT_CALLS = frozenset(("call0", "call4", "call8", "call12"))
INDIRECT_CALLS = frozenset(("callx0", "callx4", "callx8", "callx12"))
NAMED_TARGET = re.compile(r"([\da-f]+)\s+<([^>]+)>")
LITERAL_TARGET = re.compile(r"\(" + NAMED_TARGET.pattern + r"\)")


def symbol_base(name):
    """The symbol named by an objdump reference, without its address offset."""
    return re.sub(r"[+-]0x[0-9a-f]+$", "", name)


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
            current.add(symbol_base(m.group(1)))
    return calls


def decoded_calls(decoded):
    """Return (site, target address, name) in instruction order, including tail jumps.

    An unresolved indirect call has no address and names its register.
    """
    registers = {}
    calls = []
    for site, _, mnemonic, operands in decoded:
        register = operands.split(",", 1)[0].strip()
        if mnemonic in INDIRECT_CALLS:
            target = NAMED_TARGET.search(operands)
            if target and CALL_SYMBOL.fullmatch(target[2]):
                address, name = int(target[1], 16), target[2]
            else:
                register = register.split()[0] if register else register
                address, name = registers.get(register, (None, "*" + register))
            calls.append((site, address, name))
            registers.clear()
        elif (mnemonic in DIRECT_CALLS or mnemonic == "j") and (target := NAMED_TARGET.fullmatch(operands.strip())):
            calls.append((site, int(target[1], 16), target[2]))
            if mnemonic in DIRECT_CALLS:
                registers.clear()
        elif mnemonic == "l32r" and (target := LITERAL_TARGET.search(operands)):
            registers[register] = (int(target[1], 16), target[2])
        else:
            registers.pop(register, None)
    return calls
