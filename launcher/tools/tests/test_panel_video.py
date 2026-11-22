"""Panel video decoding, device-time resampling and encoding."""
import csv
import io
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

DEVICE = Path(__file__).resolve().parents[1] / "device"
sys.path.insert(0, str(DEVICE))
import panel_video as video
import video_encode
from build.c_header import read_define

FIXTURE = DEVICE.parents[1] / "test/fixtures/panelcap_fixture.h"
RED, GREEN, BLUE, BLACK = (bytes.fromhex(pixel) for pixel in ("f800", "07e0", "001f", "0000"))
RED_RGB, BLUE_RGB, BLACK_RGB = b"\xff\0\0", b"\0\0\xff", b"\0" * video.RGB_BYTES


def window(rect, pixel):
    return video.WINDOW.pack(*rect) + b"\0" + pixel


def present(seq=1, us=0, *windows):
    windows = windows or (window((0, 0, 1, 1), RED),)
    return video.PRESENT.pack(seq, us, len(windows), 2) + b"".join(windows)


def fixture_bytes(name):
    text = FIXTURE.read_text().split(f"{name}[] = {{", 1)[1].split("};", 1)[0]
    return bytes.fromhex(" ".join(re.findall(r"0x([0-9a-f]{2})", text)))


class PanelVideoTests(unittest.TestCase):
    def test_c_fixture_multi_window_multipart_with_noop(self):
        wire = fixture_bytes("panelcap_fixture_wire")
        room = read_define(FIXTURE, "PANELCAP_FIXTURE_PART_BYTES") - video.PART.size
        parts = video.Parts()
        result = None
        for index, start in enumerate(range(0, len(wire), room)):
            self.assertIsNone(result)
            result = parts.feed(video.PART.pack(1, index, start + room >= len(wire)) + wire[start:start + room])
        self.assertEqual(result, wire)
        canvas = video.Canvas()
        self.assertEqual(canvas.apply(result)[:2], (1, 0))
        raw = fixture_bytes("panelcap_fixture_pixels")
        colors = {RED: RED_RGB, BLUE: BLUE_RGB, GREEN: b"\0\xff\0", BLACK: BLACK_RGB}
        at = 0
        while at < len(raw):
            row, width = struct.unpack_from("<HH", raw, at)
            at += struct.calcsize("<HH")
            end = at + width * video.PIXEL_BYTES
            expected = b"".join(colors[raw[i:i + video.PIXEL_BYTES]] for i in range(at, end, video.PIXEL_BYTES))
            self.assertEqual(canvas.rows[row], expected + BLACK_RGB * (canvas.width - width))
            at = end

    def test_packbits_truncated_literal_and_output_overruns(self):
        for data, pixels in ((b"\x00\x01", 1), (b"\x01\0\0\0\0", 1), (b"\x81\0\0", 127), (b"", 1)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                video.unpack(data, 0, pixels)

    def test_canvas_overlaps_turn_and_atomic_rejection(self):
        canvas = video.Canvas(2, 3)
        first = window((0, 0, 1, 1), RED)
        self.assertEqual(canvas.apply(present(1, 0, first, window((0, 0, 1, 1), BLUE))), (1, 0, 2, 6, 2))
        self.assertEqual(canvas.frame(), BLACK_RGB * 3 + BLUE_RGB + BLACK_RGB * 2)
        old = canvas.frame()
        for rect in ((2, 1, 3, 2), (1, 3, 2, 4), (1, 1, 3, 2), (1, 2, 2, 4)):
            with self.subTest(rect=rect), self.assertRaises(ValueError):
                canvas.apply(present(2, 10, first, window(rect, RED)))
            self.assertEqual(canvas.frame(), old)
        self.assertEqual(canvas.apply(present(2, 10, window((1, 2, 2, 3), RED)))[0], 2)
        self.assertEqual(canvas.frame(), BLACK_RGB * 2 + RED_RGB + BLUE_RGB + BLACK_RGB * 2)

    def test_parts_rejoin_and_lost_out_of_order(self):
        stream = present()
        parts = video.Parts()
        self.assertIsNone(parts.feed(video.PART.pack(1, 0, 0) + stream[:10]))
        self.assertEqual(parts.feed(video.PART.pack(1, 1, 1) + stream[10:]), stream)
        self.assertIsNone(parts.feed(video.PART.pack(2, 0, 0) + b"abc"))
        self.assertIsNone(parts.feed(video.PART.pack(2, 2, 1) + b"def"))
        self.assertIsNone(parts.feed(video.PART.pack(2, 1, 1) + b"def"))
        self.assertEqual(parts.dropped, 1)
        self.assertEqual(parts.feed(video.PART.pack(3, 0, 1) + present(3)), present(3))
        self.assertIsNone(parts.feed(video.PART.pack(4, 1, 1) + present(4)))
        self.assertEqual(parts.dropped, 2)

    def test_resample_idle_burst_first_finish_and_backwards(self):
        frames = []
        snapshot = mock.Mock(return_value=b"a")
        sample = video.Resample(snapshot, frames.append)
        sample.present(10, 100)
        self.assertEqual(frames, [])
        sample.present(11, 100_100)
        self.assertEqual(frames, [b"a"] * 3)
        snapshot.return_value = b"b"
        sample.present(12, 110_100)
        snapshot.return_value = b"c"
        sample.present(13, 120_100)
        snapshot.return_value = b"d"
        with self.assertRaisesRegex(ValueError, "time went backwards"):
            sample.present(14, 100)
        with self.assertRaisesRegex(ValueError, "sequence did not advance"):
            sample.present(11, 125_100)
        sample.present(14, 200_100)
        self.assertEqual(frames, [b"a"] * 3 + [b"b", b"d", b"d"])
        snapshot.return_value = b"e"
        sample.finish()
        sample.finish()
        self.assertEqual(frames, [b"a"] * 3 + [b"b", b"d", b"d", b"e"])
        self.assertEqual(snapshot.call_count, 4)

    def test_csv_device_time_and_encoder_options(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "run.mp4"
            process = mock.Mock()
            process.wait.return_value = 0
            with mock.patch.object(video_encode, "resolve", return_value="fake-ffmpeg"), \
                    mock.patch.object(video_encode.subprocess, "Popen", return_value=process) as popen:
                with video.Recording(out, log=io.StringIO()) as recording:
                    with mock.patch.object(video.time, "time_ns", side_effect=[123, 999_000_000_000]):
                        recording.packet(video.PART.pack(1, 0, 1) + present(1, 0))
                        recording.packet(video.PART.pack(2, 0, 1) + present(2, 100_000))
                    recording.finish("PANELCAP_END presents=2 dropped=4 bytes=54")
                command = popen.call_args.args[0]
                self.assertIn("libx264", command)
                self.assertIn("zerolatency", command)
                self.assertEqual(command[command.index("-g") + 1], "30")
                self.assertEqual(command[command.index("-r") + 1], "30")
                self.assertEqual([command[i + 1] for i, arg in enumerate(command) if arg == "-pix_fmt"],
                                 ["rgb24", "yuv420p"])
                self.assertEqual(process.stdin.write.call_count, 4)
                if sys.platform == "win32":
                    self.assertEqual(popen.call_args.kwargs["creationflags"], subprocess.CREATE_NEW_PROCESS_GROUP)
                else:
                    self.assertTrue(popen.call_args.kwargs["start_new_session"])
            with out.with_suffix(".frames.csv").open() as source:
                rows = list(csv.reader(source))
            self.assertEqual(rows, [list(video.CSV_FIELDS), ["1", "0", "123", "1", "3", "2"],
                                    ["2", "100000", "999000000000", "1", "3", "2"]])

    def test_closing_broken_stdin_waits_and_reports_status(self):
        with mock.patch.object(video_encode, "resolve", return_value="fake-ffmpeg"):
            writer = video_encode.VideoWriter("test.mp4", 30)
        writer.process = mock.Mock()
        writer.process.stdin.close.side_effect = BrokenPipeError
        writer.process.wait.return_value = 7
        with self.assertRaisesRegex(RuntimeError, "ffmpeg exited with status 7"):
            writer.close()
        writer.process.wait.assert_called_once()

    def test_real_three_frame_encode(self):
        if video_encode.find() is None:
            self.skipTest("ffmpeg is absent")
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "run.mp4"
            writer = video_encode.VideoWriter(out, 30, live=True)
            for pixel in (RED_RGB, BLUE_RGB, BLACK_RGB):
                writer.write((2, 2), pixel * 4)
            writer.close()
            rgb = subprocess.check_output([writer.binary, "-v", "error", "-i", str(out), "-f", "rawvideo",
                                           "-pix_fmt", "rgb24", "-"])
            self.assertEqual(len(rgb), 3 * 2 * 2 * video.RGB_BYTES)

    def test_ffmpeg_pin_missing_patch_note_and_major_refusal(self):
        with mock.patch.object(video_encode, "find", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "ffmpeg is missing"):
                video_encode.resolve(io.StringIO())
        with mock.patch.object(video_encode, "pinned", return_value={"version": "6.1.1"}), \
                mock.patch.object(video_encode, "find", return_value="ffmpeg"):
            with mock.patch.object(video_encode, "version", return_value="6.1.2"):
                log = io.StringIO()
                self.assertEqual(video_encode.resolve(log), "ffmpeg")
                self.assertIn("6.1.2; pinned 6.1.1", log.getvalue())
            with mock.patch.object(video_encode, "version", return_value="7.0"):
                with self.assertRaisesRegex(RuntimeError, "7.0; pinned 6.1.1"):
                    video_encode.resolve(io.StringIO())


if __name__ == "__main__":
    unittest.main()
