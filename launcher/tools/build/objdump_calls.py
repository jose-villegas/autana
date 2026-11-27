"""Who calls whom, read from the Xtensa objdump's disassembly."""

import re

FUNCTION = re.compile(r"^([0-9a-f]+) <([^>]+)>:\s*$")
SECTION = re.compile(r"^Disassembly of section (.+):$")
INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+([a-z][a-z0-9.]*)\s*(.*)$")
RELOCATION = re.compile(r"^\s*([0-9a-f]+):\s+R_XTENSA_(\w+)\s+(\S+)")
SYMBOL_OFFSET = re.compile(r"([+-]0x[0-9a-f]+)$")

CALL_SYMBOL = re.compile(r"[\w.$@]+")
DIRECT_CALLS = frozenset(("call0", "call4", "call8", "call12"))
INDIRECT_CALLS = frozenset(("callx0", "callx4", "callx8", "callx12"))
STORES = frozenset(("s32i", "s16i", "s8i", "ssi"))
NAMED_TARGET = re.compile(r"([\da-f]+)\s+<([^>]+)>")
LITERAL_TARGET = re.compile(r"\(" + NAMED_TARGET.pattern + r"\)")


def symbol_base(name):
    """The symbol named by an objdump reference, without its address offset."""
    return SYMBOL_OFFSET.sub("", name)


def function_addresses(text):
    """{function: address} for every function header in `objdump -d` text."""
    found = {}
    for line in text.splitlines():
        m = FUNCTION.match(line)
        if m:
            found.setdefault(m.group(2), int(m.group(1), 16))
    return found


def parse_objdump(text):
    """Decode instruction lines as (address, byte count, mnemonic, operands)."""
    return [(int(match[1], 16), len(match[2]) // 2, match[3], match[4])
            for line in text.splitlines() if (match := INSTRUCTION.match(line))]


def parse_calls(text, relocation=None):
    """Group direct linked calls or relocatable object calls by function."""
    calls = {}
    for function, _, _, name in disassembly_calls(text, relocation):
        calls.setdefault(function, set()).add(name)
    return calls


def disassembly_calls(text, relocation=None):
    """Yield (function, section, site, target name) from objdump disassembly."""
    blocks = {}
    literals = {}
    section = None
    body = None
    for line in text.splitlines():
        if match := SECTION.match(line):
            section = match[1]
            body = None
        elif match := FUNCTION.match(line):
            body, relocations = blocks.setdefault((section, match[2]), ([], {}))
        elif body is not None:
            if relocation is not None and (match := relocation.match(line)):
                site, kind, name = int(match[1], 16), match[2], match[3]
                relocations.setdefault(site, {})[kind] = name
                if kind == "32":
                    literals[section, site] = name
            else:
                body.append(line)

    for (section, function), (body, relocations) in blocks.items():
        instructions = parse_objdump("\n".join(body))
        if section is None and not instructions:
            # A relocation-only excerpt has no instructions to locate its calls.
            for site, names in relocations.items():
                for name in names.values():
                    yield function.removeprefix(".literal."), section, site, symbol_base(name)
        if section and section.startswith(".literal."):
            continue
        fixups = {}
        for site, _, mnemonic, _ in instructions:
            names = relocations.get(site, {})
            # An expanded long call's symbol is on the l32r that feeds callx.
            name = names.get("ASM_EXPAND") or names.get("SLOT0_OP")
            if name and mnemonic == "l32r" and "ASM_EXPAND" not in names:
                offset = SYMBOL_OFFSET.search(name)
                name = literals.get((symbol_base(name), int(offset[1], 16) if offset else 0))
            if name:
                fixups[site] = name
        direct = {site for site, _, mnemonic, _ in instructions if mnemonic in DIRECT_CALLS}
        for site, _, name in decoded_calls(instructions, fixups):
            if relocation is not None or (site in direct and "+" not in name):
                yield function, section, site, symbol_base(name)


def decoded_calls(decoded, relocations=None):
    """Return (site, target address, name) in instruction order, including tail jumps.

    Object relocations supply names without addresses. An unresolved indirect
    call names its register.
    """
    relocations = relocations or {}
    registers = {}
    calls = []
    for site, _, mnemonic, operands in decoded:
        register = operands.split(",", 1)[0].strip()
        relocated = relocations.get(site)
        if mnemonic in INDIRECT_CALLS:
            target = NAMED_TARGET.search(operands)
            if relocated:
                address, name = None, relocated
            elif target and CALL_SYMBOL.fullmatch(target[2]):
                address, name = int(target[1], 16), target[2]
            else:
                register = register.split()[0] if register else register
                address, name = registers.get(register, (None, "*" + register))
            calls.append((site, address, name))
            registers.clear()
        elif mnemonic in DIRECT_CALLS or mnemonic == "j":
            target = NAMED_TARGET.fullmatch(operands.strip())
            if relocated:
                calls.append((site, None, relocated))
            elif target:
                calls.append((site, int(target[1], 16), target[2]))
            if mnemonic in DIRECT_CALLS:
                registers.clear()
        elif mnemonic == "l32r" and relocated:
            registers[register] = (None, relocated)
        elif mnemonic == "l32r" and (target := LITERAL_TARGET.search(operands)):
            registers[register] = (int(target[1], 16), target[2])
        elif mnemonic in ("mov", "mov.n") and (source := operands.partition(",")[2].strip()) in registers:
            registers[register] = registers[source]
        elif mnemonic.removesuffix(".n") not in STORES:
            registers.pop(register, None)
    return calls
