"""Panel capture packets carry the bytes of a forced known-pattern present."""
from pathlib import Path
import re
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/device"))
import panel_video

CAPTURE_SECONDS = 30
TEST_RGB565BE = b"\x22\x0c"


def test_panelcap_known_pattern(console):
    dut = console.dut
    console.exchange("HOME", r"HOME_OK[^\r\n]*")
    console.exchange("FREEZE", r"FREEZE_STATE frozen=1 steps=0")
    started = False
    try:
        console.exchange("PANELCAP START 60", r"PANELCAP_BEGIN fps=60 image=\d+")
        started = True
        dut.write("RUNSUITE suite_panelcap_pattern")
        parts = panel_video.Parts()
        deadline = time.monotonic() + CAPTURE_SECONDS
        found = False
        while time.monotonic() < deadline:
            match = dut.expect(rb"PANEL_BIN (\d+) ([0-9a-fA-F]+)\r?\n", timeout=CAPTURE_SECONDS)
            head = re.search(rb"PANEL_BIN (\d+) ([0-9a-fA-F]+)", match[0])
            size, crc = int(head[1]), int(head[2], 16)
            packet = dut.expect(re.compile(rb"(?s).{" + str(size).encode() + rb"}"), timeout=CAPTURE_SECONDS)[0]
            assert zlib.crc32(packet) == crc
            stream = parts.feed(packet)
            if stream is None:
                continue
            seq, us, windows, dropped = panel_video.PRESENT.unpack_from(stream)
            assert seq > 0 and us > 0 and dropped == 0
            at = panel_video.PRESENT.size
            for _ in range(windows):
                x0, y0, x1, y1, packed = panel_video.WINDOW.unpack_from(stream, at)
                at += panel_video.WINDOW.size
                pixels = (x1 - x0) * (y1 - y0)
                assert panel_video.unpack(stream[at:at + packed], pixels) == TEST_RGB565BE * pixels
                at += packed
            assert windows > 0 and at == len(stream)
            found = True
            break
        assert found, "no complete PANEL present before capture deadline"
    finally:
        try:
            if started:
                console.exchange("PANELCAP STOP", r"PANELCAP_END presents=\d+ dropped=\d+ bytes=\d+", timeout=CAPTURE_SECONDS)
        finally:
            console.exchange("RESUME", r"FREEZE_STATE frozen=0 steps=0")
