#!/usr/bin/env python3
"""Generate flash-resident slicing tables for the reflected RFC 1952 polynomial."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "launcher/packages/hash/src/crc32_tables.inc"
POLYNOMIAL = 0xEDB88320
BYTE_BITS = 8
SLICE_BYTES = 4


def generate():
    tables = []
    for lane in range(SLICE_BYTES):
        table = []
        for value in range(1 << BYTE_BITS):
            crc = value if lane == 0 else tables[-1][value]
            for _ in range(BYTE_BITS):
                crc = (crc >> 1) ^ (POLYNOMIAL if crc & 1 else 0)
            table.append(crc)
        tables.append(table)
    assert tables[0][1 << (BYTE_BITS - 1)] == POLYNOMIAL
    lines = ["/* GENERATED FILE - do not edit.",
             " * python launcher/tools/gen/gen_crc32_tables.py launcher/packages/hash/src/crc32_tables.inc",
             " */",
             f"static const uint32_t crc32_slice[{SLICE_BYTES}][{1 << BYTE_BITS}] = {{"]
    for table in tables:
        lines.append("    {")
        for start in range(0, len(table), SLICE_BYTES):
            lines.append("        " + ", ".join(f"0x{value:08X}U" for value in table[start:start + SLICE_BYTES]) + ",")
        lines.append("    },")
    return "\n".join([*lines, "};", ""])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    expected = generate()
    if args.check:
        if args.output.read_text() != expected:
            parser.exit(1, "CRC-32 tables are stale\n")
    else:
        args.output.write_bytes(expected.encode())


if __name__ == "__main__":
    main()
