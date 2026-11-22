"""Verify panel video across the console, encoder and persistent RGB canvas."""
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/device"))
import console_packets
import panel_video
from build.c_header import read_defines

CAPTURE_SECONDS = 30
READ_BYTES = 4096
SUITE = "suite_panelcap_pattern"
SUITE_SOURCE = Path(__file__).resolve().parents[1] / "suites/suite_panelcap.c"
PATTERN_ROWS, Y, H = read_defines(SUITE_SOURCE, "PANELCAP_TEST_", ("ROWS", "Y", "H"))
IMAGE_BYTES = panel_video.screenshot.PANEL_WIDTH * panel_video.screenshot.PANEL_HEIGHT * panel_video.PIXEL_BYTES
RED, GREEN, BLUE =b"\xff\x00\x00", b"\x00\xff\x00", b"\x00\x00\xff"


class Capture:
    def __init__(self, dut):
        self.dut = dut
        self.packets = console_packets.Packets(panel_video.PACKET_TAG)
        self.parts = panel_video.Parts()
        self.canvas = panel_video.Canvas()
        self.totals = [0, 0, 0]

    def events(self):
        proc = self.dut.pexpect_proc
        chunk = proc.buffer
        proc.buffer = b""
        deadline = time.monotonic() + CAPTURE_SECONDS
        while time.monotonic() < deadline:
            yield from self.packets.feed(chunk)
            chunk = proc.read_nonblocking(READ_BYTES, timeout=max(0, deadline - time.monotonic()))
        raise AssertionError("PANEL capture deadline reached")

    def frame(self, payload):
        stream = self.parts.feed(payload)
        if stream is None:
            return None
        seq, us, windows, _, dropped = self.canvas.apply(stream)
        self.totals[0] += 1
        self.totals[1] += dropped
        self.totals[2] += len(stream)
        assert self.parts.dropped == 0
        assert us > 0 and windows == 1
        return seq, panel_video.WINDOW.unpack_from(stream, panel_video.PRESENT.size)

    def end(self, line):
        counts = re.fullmatch(rb"PANELCAP_END presents=(\d+) dropped=(\d+) bytes=(\d+) stop_us=\d+", line)
        assert counts, line
        assert list(map(int, counts.groups())) == self.totals

    def stop_during_full_pattern(self):
        self.dut.write(f"RUNSUITE {SUITE} full_pattern")
        stopping = False
        for kind, payload in self.events():
            if kind == "data":
                if not stopping:
                    assert panel_video.PART.unpack_from(payload)[1:] == (0, 0)
                    self.dut.write("PANELCAP STOP")
                    stopping = True
                frame = self.frame(payload)
                if frame is not None:
                    assert frame[0] == 1 and self.totals[1] == 0
            elif b":FAIL" in payload:
                raise AssertionError(payload)
            elif payload.startswith(b"PANELCAP_END"):
                assert stopping and self.totals[0] == 1
                self.end(payload)
                return


def test_panelcap_full_then_partial(console):
    dut = console.dut
    console.exchange("HOME", r"HOME_OK[^\r\n]*")
    console.exchange("FREEZE", r"FREEZE_STATE frozen=1 steps=0")
    fps = panel_video.read_define(panel_video.HEADER, "PANELCAP_FPS_MAX")
    start = f"PANELCAP START {fps}"
    begin = rf"PANELCAP_BEGIN fps={fps} image={IMAGE_BYTES}"
    started = False
    try:
        console.exchange(start, begin)
        started = True
        capture = Capture(dut)
        dut.write(f"RUNSUITE {SUITE} full_pattern")
        events = capture.events()
        full = partial = None
        completed = 0
        for kind, payload in events:
            if kind == "data":
                result = capture.frame(payload)
                if result is None:
                    continue
                if full is None:
                    full, rect = result
                    assert full == 1 and rect == (0, 0, capture.canvas.width, capture.canvas.height)
                    assert capture.parts.part > 1
                    alternate = (RED + GREEN) * (capture.canvas.width // 2)
                    assert capture.canvas.rows == ([alternate] * PATTERN_ROWS
                                                   + [BLUE * capture.canvas.width] * (capture.canvas.height - PATTERN_ROWS))
                else:
                    raise AssertionError("unexpected present before the partial update")
            elif b":FAIL" in payload:
                raise AssertionError(payload)
            elif payload.startswith(b"RUNSUITE_COMPLETE"):
                completed += 1
            if full is not None and completed == 1:
                dut.write(f"RUNSUITE {SUITE} partial_pattern")
                break
        for kind, payload in events:
            if kind == "data":
                result = capture.frame(payload)
                if result is not None:
                    partial, rect = result
                    assert partial == full + 1 and rect == (0, Y, capture.canvas.width, Y + H)
                    for y, row in enumerate(capture.canvas.rows):
                        expected = alternate if y < PATTERN_ROWS and not Y <= y < Y + H else BLUE * capture.canvas.width
                        assert row == expected
            elif b":FAIL" in payload:
                raise AssertionError(payload)
            elif payload.startswith(b"RUNSUITE_COMPLETE"):
                completed += 1
            if partial is not None and completed == 2:
                dut.write("PANELCAP STOP")
                break
        for kind, payload in events:
            if kind == "line" and payload.startswith(b"PANELCAP_END"):
                started = False
                capture.end(payload)
                break
        console.exchange(start, begin)
        started = True
        restarted = Capture(dut)
        restarted.stop_during_full_pattern()
        started = False
        assert capture.totals[0] == 2
        assert capture.totals[2] > restarted.totals[2]
    finally:
        if started:
            console.exchange("PANELCAP STOP", r"PANELCAP_(?:END|REFUSED)[^\r\n]*", timeout=CAPTURE_SECONDS)
        console.exchange("RESUME", r"FREEZE_STATE frozen=0 steps=0")


def test_panelcap_verbs_and_stop_drains(console):
    dut = console.dut
    default, maximum = panel_video.read_defines(panel_video.HEADER, "PANELCAP_FPS_", ("DEFAULT", "MAX"))
    console.exchange("HOME", r"HOME_OK[^\r\n]*")
    console.exchange("FREEZE", r"FREEZE_STATE frozen=1 steps=0")
    try:
        console.exchange("PANELCAP", r"PANELCAP_REFUSED: usage[^\r\n]*")
        console.exchange("PANELCAP STOP", "PANELCAP_REFUSED: not recording")
        for invalid in (0, maximum + 1):
            console.exchange(f"PANELCAP START {invalid}", "PANELCAP_REFUSED: bad fps")
        console.exchange("PANELCAP START", rf"PANELCAP_BEGIN fps={default} image={IMAGE_BYTES}")
        console.exchange("PANELCAP START", "PANELCAP_REFUSED: already on")
        Capture(dut).stop_during_full_pattern()
        console.exchange("PANELCAP STOP", "PANELCAP_REFUSED: not recording")
    finally:
        console.exchange("RESUME", r"FREEZE_STATE frozen=0 steps=0")


def test_panelcap_app_switch_drains(console):
    console.exchange("HOME", r"HOME_OK[^\r\n]*")
    dut = console.dut
    suite = "suite_panelcap_lifecycle"
    dut.write(f"RUNSUITE {suite} app_switch_stops_capture")
    for _ in range(2):
        dut.expect_exact(panel_video.read_define(panel_video.CONSOLE_HEADER, "PANELCAP_APP_SWITCH").encode())
        dut.expect(rb"PANELCAP_END presents=0 dropped=0 bytes=0")
    dut.expect(rb"test_app_switch_stops_capture:PASS")
    dut.expect(rb"RUNSUITE_COMPLETE name=suite_panelcap_lifecycle found=1[^\r\n]*")
