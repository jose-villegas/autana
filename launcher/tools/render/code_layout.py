#!/usr/bin/env python3
"""Report cache-line offsets of renderer functions and their machine loops."""

import argparse
import re
import subprocess


FUNCTIONS = (
    "raster_draw",
    "raster_upscale",
    "r3d_pipeline_draw",
    "r3d_span_triangle_impl",
    "upscale_rows",
    "walk_rows",
    "r3d_span_hidden",
)


def run(tool, *arguments):
    return subprocess.run([tool, *arguments], check=True, capture_output=True, text=True).stdout


def symbols(nm, elf):
    found = {}
    for line in run(nm, "-n", "-S", elf).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[3] in FUNCTIONS:
            found[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
    return found


def instructions(objdump, elf, function):
    found = []
    for line in run(objdump, "-d", f"--disassemble={function}", elf).splitlines():
        match = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+([a-z][a-z0-9.]*)\s*(.*)$", line)
        if match:
            found.append((int(match[1], 16), len(match[2]) // 2, match[3], match[4]))
    return found


def target_of(operands):
    match = re.search(r"\b([0-9a-f]{4,})\s+<", operands)
    return int(match[1], 16) if match else None


def line_span(start, end, line_size):
    return ((end - 1) // line_size) - (start // line_size) + 1


def report(label, elf, nm, objdump, line_size):
    located = symbols(nm, elf)
    print(f"## {label}")
    print("| function | address | mod 32 | bytes |")
    print("|---|---:|---:|---:|")
    for function in FUNCTIONS:
        address, size = located[function]
        print(f"| {function} | 0x{address:08x} | {address % line_size} | {size} |")
    print()
    print("| function | kind | start | mod 32 | bytes | lines |")
    print("|---|---|---:|---:|---:|---:|")
    for function in FUNCTIONS:
        code = instructions(objdump, elf, function)
        for index, (address, size, mnemonic, operands) in enumerate(code):
            target = target_of(operands)
            if mnemonic in ("loop", "loopnez", "loopgtz") and target is not None:
                start = address + size
                span = target - start
                print(
                    f"| {function} | {mnemonic} | 0x{start:08x} | {start % line_size} | {span} | "
                    f"{line_span(start, target, line_size)} |"
                )
            elif target is not None and target < address and (mnemonic == "j" or mnemonic.startswith("b")):
                end = address + size
                print(
                    f"| {function} | {mnemonic} back | 0x{target:08x} | {target % line_size} | {end - target} | "
                    f"{line_span(target, end, line_size)} |"
                )
        print()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--line-size", type=int, default=32)
    parser.add_argument("images", nargs="+", metavar="LABEL=ELF")
    args = parser.parse_args()
    for image in args.images:
        label, elf = image.split("=", 1)
        report(label, elf, args.nm, args.objdump, args.line_size)


if __name__ == "__main__":
    main()
