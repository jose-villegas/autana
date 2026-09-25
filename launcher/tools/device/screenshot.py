"""The screenshot wire protocol and decoder: a pure library, imported by
scripts/device/device.py's own `screenshot` subcommand (autana's `autana
screenshot`) so there is exactly one decoder to drift out of sync with the
device. Opening the port is the caller's job - see device.py's own capture,
which does it under the device lock - so this module never touches one
itself.

Sends the trigger word over the console UART (see main/console/console.c)
and reads the response back out of the same stream idf_monitor would
otherwise be showing as logs: a SCREENSHOT_BEGIN line announcing the byte
count, one SCREENSHOT_DATA: line per base64-encoded chunk, one
SCREENSHOT_STATE: line of plain-text JSON (device state at that same frame
- sensors, memory, clock; see console_screenshot_dump()'s own comment in
main/console/console_screenshot.c for the field list), and a
SCREENSHOT_END line - or, in place of all of those, one
SCREENSHOT_REFUSED: line giving the reason, which ends the run at once
rather than at the timeout. Anything else on the wire - ordinary
ESP_LOG output, in particular - is ignored rather than treated as an
error, since the device keeps logging normally while it streams.

The device streams its frame as a 24bpp BMP (see screenshot_bmp_header() in
util/screenshot.h) - the simplest thing to emit from a microcontroller with
no image library on it - but nothing here ever writes that BMP to disk:
bmp_bytes_to_png() below converts it to PNG entirely in memory, and a
capture's output gets only the PNG. This is genuinely lossless, not just
smaller - PNG's compression is DEFLATE, the same as zlib/gzip, so every pixel
round-trips exactly; this is not JPEG. Standard library only (zlib +
struct), no Pillow - Pillow is not installed in the ESP-IDF python env this
module actually runs under, so depending on it would silently produce no
image at all. write_capture() also writes a same-named .json beside the
.png if a SCREENSHOT_STATE: line arrived.
"""

import base64
import json
import os
import re
import struct
import time
import zlib

BEGIN_RE = re.compile(r"^SCREENSHOT_BEGIN size=(\d+)$")
DATA_PREFIX = "SCREENSHOT_DATA:"
STATE_PREFIX = "SCREENSHOT_STATE:"
REFUSED_PREFIX = "SCREENSHOT_REFUSED:"
END_LINE = "SCREENSHOT_END"

TRIGGER = b"SCREENSHOT\n"


class ScreenshotRefused(RuntimeError):
    """The device answered with a SCREENSHOT_REFUSED: line - its own message
    is this exception's, e.g. "band mode, and no room in PSRAM..."."""


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    """One PNG chunk: 4-byte big-endian length, the 4-byte type, the data,
    then a big-endian CRC32 over type+data - the whole file is just these
    end to end after the fixed 8-byte signature."""
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))


def bmp_bytes_to_png(bmp: bytes) -> bytes:
    """Converts an in-memory 24bpp BMP - the exact bytes screenshot_dump()
    streams, see screenshot_bmp_header()'s own comment for the byte layout
    - into an in-memory PNG. No Pillow, no temp file: a minimal PNG is just
    the 8-byte signature, an IHDR chunk, one IDAT chunk holding
    zlib.compress() of the raw scanlines (each prefixed with a filter byte;
    0 = "None" is correct and simplest here), and an empty IEND chunk.

    Two orderings BMP and PNG disagree on, both handled below: BMP stores
    rows bottom-up (screenshot_bmp_header() always writes a positive
    biHeight - see its own comment) while PNG wants top-down, and BMP's
    pixel order is B,G,R while PNG wants R,G,B. Getting either backwards
    produces an image that LOOKS like a real screenshot - upside-down, or
    blue-tinted - which is worse than no image at all.

    Width/height/row-stride are read out of the BMP header rather than
    assumed, so this keeps working unchanged if the panel resolution ever
    does - the same "trust what the device announced" reasoning the
    SCREENSHOT_BEGIN size check in read_screenshot() already applies.
    """
    if bmp[0:2] != b"BM":
        raise ValueError("not a BMP: missing the 'BM' signature")

    pixel_offset, = struct.unpack_from("<I", bmp, 10)
    width, height = struct.unpack_from("<ii", bmp, 18)
    bits_per_pixel, = struct.unpack_from("<H", bmp, 28)
    if bits_per_pixel != 24:
        raise ValueError(f"expected a 24bpp BMP, got {bits_per_pixel}bpp")
    if height <= 0:
        # screenshot_bmp_header() only ever writes a positive (bottom-up)
        # height - a value <= 0 here means this isn't this tool's BMP.
        raise ValueError(f"expected a positive (bottom-up) height, got {height}")

    stride = ((width * 3 + 3) // 4) * 4     # BMP pads every row to 4 bytes

    scanlines = []
    for image_row in range(height):
        # BMP's first stored row is the BOTTOM of the image; PNG's first
        # written row is the TOP - so PNG row N reads BMP's stored row
        # (height - 1 - N).
        bmp_row = height - 1 - image_row
        row_start = pixel_offset + bmp_row * stride
        bgr = bmp[row_start:row_start + width * 3]

        rgb = bytearray(len(bgr))
        rgb[0::3], rgb[1::3], rgb[2::3] = bgr[2::3], bgr[1::3], bgr[0::3]
        scanlines.append(b"\x00" + bytes(rgb))    # filter byte 0 = None

    raw = b"".join(scanlines)

    png = b"\x89PNG\r\n\x1a\n"
    png += _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += _png_chunk(b"IDAT", zlib.compress(raw, 6))
    png += _png_chunk(b"IEND", b"")
    return png


def turn_png(png: bytes, quarter: int) -> bytes:
    """Turn an RGB PNG made by bmp_bytes_to_png() clockwise by `quarter`."""
    quarter %= 4
    if quarter == 0:
        return png

    pos = 8
    header = None
    compressed = bytearray()
    while pos < len(png):
        length, = struct.unpack_from(">I", png, pos)
        tag = png[pos + 4:pos + 8]
        data = png[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            header = struct.unpack(">IIBBBBB", data)
        elif tag == b"IDAT":
            compressed += data
        pos += length + 12

    if header is None:
        raise ValueError("not a PNG: no IHDR chunk")
    width, height, depth, colour, compression, filter_method, interlace = header
    if (depth, colour, compression, filter_method, interlace) != (8, 2, 0, 0, 0):
        raise ValueError("expected an 8-bit RGB PNG without interlace")

    raw = zlib.decompress(compressed)
    stride = width * 3
    rows = []
    for y in range(height):
        start = y * (stride + 1)
        if raw[start] != 0:
            raise ValueError("expected unfiltered PNG rows")
        rows.append(raw[start + 1:start + 1 + stride])

    if quarter == 1:
        rows = [b"".join(rows[height - 1 - x][y * 3:y * 3 + 3] for x in range(height))
                for y in range(width)]
    elif quarter == 2:
        rows = [b"".join(row[x * 3:x * 3 + 3] for x in range(width - 1, -1, -1))
                for row in reversed(rows)]
    else:
        rows = [b"".join(rows[x][(width - 1 - y) * 3:(width - y) * 3] for x in range(height))
                for y in range(width)]

    raw = b"".join(b"\0" + row for row in rows)
    out_width, out_height = (height, width) if quarter % 2 else (width, height)
    turned = b"\x89PNG\r\n\x1a\n"
    turned += _png_chunk(b"IHDR", struct.pack(">IIBBBBB", out_width, out_height, 8, 2, 0, 0, 0))
    turned += _png_chunk(b"IDAT", zlib.compress(raw, 6))
    turned += _png_chunk(b"IEND", b"")
    return turned


def read_screenshot(port, timeout, on_status=None):
    """Trigger one capture on an already-open connection and return
    (png_bytes, state_json_or_None).

    `port` is only read from and written to - opening it (including the
    DTR/RTS setup that keeps the board from resetting, see device.py's
    open_serial()) and closing it stay the caller's, since a capture is one
    thing to do on a connection already open under whatever the caller's own
    reason for holding it is (a bare port here, the device lock there).
    Raises ScreenshotRefused if the device refuses, RuntimeError if a
    complete capture never arrives within `timeout`. `on_status(message)` is
    called with each progress line, defaulting to nothing.
    """
    if on_status is None:
        def on_status(message):
            pass

    deadline = time.monotonic() + timeout
    buffer = ""
    total_size = None
    chunks = []
    received_b64_chars = 0
    state_json = None

    # Printed immediately, before anything blocks: without this, a slow but
    # perfectly healthy capture prints nothing at all until it either
    # finishes or times out, and looks identical to a genuine hang for the
    # whole timeout in between.
    on_status(f"sent trigger, waiting for the device (up to {timeout:g}s for a full frame)...")
    last_progress = time.monotonic()

    port.reset_input_buffer()
    port.write(TRIGGER)
    port.flush()
    last_trigger_sent = time.monotonic()

    while time.monotonic() < deadline:
        chunk = port.read(4096)
        if chunk:
            buffer += chunk.decode("utf-8", errors="replace")

        now = time.monotonic()

        # Resend the trigger periodically until SCREENSHOT_BEGIN shows up. A
        # single lost byte is easy to hit right after flashing: flashing
        # resets the board, and if this runs before the firmware has gotten
        # through POST and the boot animation to install its UART listener,
        # the one-shot trigger arrives before anything is reading for it and
        # is gone for good - otherwise indistinguishable from a genuine hang,
        # since the firmware comes up moments later in a perfectly normal,
        # listening state that will happily answer the NEXT one.
        if total_size is None and now - last_trigger_sent >= 5.0:
            on_status("  ... no response yet, resending trigger (the device may still be booting)")
            port.write(TRIGGER)
            port.flush()
            last_trigger_sent = now

        if now - last_progress >= 3.0:
            if total_size is None:
                on_status("  ... still waiting for SCREENSHOT_BEGIN (nothing recognizable seen yet)")
            else:
                received_bytes = received_b64_chars * 3 // 4
                pct = min(100, received_bytes * 100 // max(total_size, 1))
                on_status(f"  ... {pct}% ({received_bytes}/{total_size} bytes)")
            last_progress = now

        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            line = line.rstrip("\r")

            if line.startswith(REFUSED_PREFIX):
                raise ScreenshotRefused(line[len(REFUSED_PREFIX):].strip())

            if total_size is None:
                m = BEGIN_RE.match(line)
                if m:
                    total_size = int(m.group(1))
                    on_status(f"  capturing {total_size} bytes...")
                continue

            if line == END_LINE:
                data = base64.b64decode("".join(chunks))
                if len(data) != total_size:
                    on_status(f"warning: decoded {len(data)} bytes but the device "
                             f"announced {total_size}")
                return bmp_bytes_to_png(data), state_json

            if line.startswith(STATE_PREFIX):
                state_json = line[len(STATE_PREFIX):]
                continue

            if line.startswith(DATA_PREFIX):
                encoded = line[len(DATA_PREFIX):]
                chunks.append(encoded)
                received_b64_chars += len(encoded)
            # anything else on the wire is ordinary log output - ignored

    if total_size is None:
        raise RuntimeError(
            f"timed out after {timeout:g}s without a complete capture. never saw a "
            "SCREENSHOT_BEGIN line - is the firmware built with the screenshot listener "
            "(util/screenshot.c), and is it actually running (not stuck in the boot "
            "animation or a crash loop)?")
    raise RuntimeError(
        f"timed out after {timeout:g}s without a complete capture. saw SCREENSHOT_BEGIN "
        f"size={total_size} but never SCREENSHOT_END - the transfer started but did not finish.")


def write_capture(out, png, state_json, image_turn_quarter=None):
    """png/.json beside each other, `out`'s extension replaced with .png.
    Returns (png_path, state_path_or_None); a state_json that fails to parse as JSON
    is still written, raw, to state_path - this both validates the device's
    own formatting (screenshot.c's snprintf() is hand-rolled, not a JSON
    library - see suite_device_state.c for what IS verified, on a host, ahead
    of ever reaching real hardware) and pretty-prints it for a human reading
    the file afterward.
    """
    png_path = os.path.splitext(out)[0] + ".png"
    with open(png_path, "wb") as f:
        f.write(png)

    if state_json is None:
        return png_path, None

    state_path = os.path.splitext(out)[0] + ".json"
    try:
        parsed = json.loads(state_json)
        if image_turn_quarter is not None:
            parsed["image_turn_quarter"] = image_turn_quarter
        with open(state_path, "w") as f:
            json.dump(parsed, f, indent=2)
            f.write("\n")
    except json.JSONDecodeError:
        with open(state_path, "w") as f:
            f.write(state_json + "\n")
    return png_path, state_path
