"""Generate the shared panel capture wire fixture from explicit PackBits commands."""
from pathlib import Path
import struct

RUN = 128
PART_BYTES = 29
RED, GREEN, BLUE, BLACK = (bytes.fromhex(pixel) for pixel in ("f800", "07e0", "001f", "0000"))


def literal(*pixels):
    return bytes([len(pixels) - 1]) + b"".join(pixels)


def repeat(count, pixel):
    return bytes([1 - count & 255]) + pixel


def c_bytes(name, data):
    rows = ["    " + ", ".join(f"0x{byte:02x}" for byte in data[i:i + 12]) + ","
            for i in range(0, len(data), 12)]
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(rows) + "\n};\n"


def generate():
    incompressible = (RED, GREEN) * (RUN // 2) + (BLUE,)
    examples = [
        ((RED,) * (RUN - 1), repeat(RUN - 1, RED)),
        ((GREEN,) * RUN, repeat(RUN, GREEN)),
        ((BLUE,) * (RUN + 1), repeat(RUN, BLUE) + literal(BLUE)),
        ((RED, GREEN, GREEN, BLUE, BLACK, BLACK), literal(RED, GREEN, GREEN, BLUE, BLACK, BLACK)),
        ((RED, BLUE, BLUE, BLUE, GREEN), literal(RED) + repeat(3, BLUE) + literal(GREEN)),
        (incompressible, literal(*incompressible[:RUN]) + literal(incompressible[-1])),
    ]
    raw = bytearray()
    wire = bytearray(struct.pack("<IQHH", 1, 0, len(examples), 0))
    for index, (pixels, packed) in enumerate(examples):
        y = 2 * index
        width = len(pixels)
        raw.extend(struct.pack("<HH", y, width))
        raw.extend(b"".join(pixels))
        wire.extend(struct.pack("<HHHH", 0, y, width, y + 1))
        if index == 0:
            wire.append(RUN)
        wire.extend(packed)

    return (
        "/* panelcap_fixture: generated wire and source pixels from gen_panelcap_fixture.py. */\n"
        '#pragma once\n#include <stdint.h>\n\n'
        f"#define PANELCAP_FIXTURE_PART_BYTES {PART_BYTES}U\n"
        f"#define PANELCAP_FIXTURE_NOOP_AT    {struct.calcsize('<IQHHHHHH')}U\n\n"
        "/* clang-format off */\n"
        + c_bytes("panelcap_fixture_pixels", raw)
        + "\n" + c_bytes("panelcap_fixture_wire", wire)
        + "/* clang-format on */\n"
    )


if __name__ == "__main__":
    Path(__file__).with_name("panelcap_fixture.h").write_text(generate(), encoding="utf-8")
