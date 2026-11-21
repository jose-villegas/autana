"""Panel video decoding and device-time resampling against independent wire examples."""
import csv
import importlib.util
import io
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

DEVICE = Path(os.environ.get("PANEL_VIDEO_DEVICE_DIR", Path(__file__).resolve().parents[1] / "device"))
sys.path.insert(0, str(DEVICE))
import console_packets
import screenshot

spec = importlib.util.spec_from_file_location("panel_video", os.environ.get("PANEL_VIDEO_SOURCE", DEVICE / "panel_video.py"))
video = importlib.util.module_from_spec(spec)
spec.loader.exec_module(video)


def present(seq=1, us=0, pixel=b"\xf8\x00"):
    return struct.pack("<IQHH", seq, us, 1, 2) + struct.pack("<HHHHI", 0, 0, 1, 1, 3) + b"\x00" + pixel


class PanelVideoTests(unittest.TestCase):
    def test_packbits_literal_repeat_noop_and_errors(self):
        self.assertEqual(video.unpack(b"\x80\x00\x12\x34\x81\xab\xcd", 129), b"\x12\x34" + b"\xab\xcd" * 128)
        for data, pixels in ((b"\x00\x01", 1), (b"\x81\x00\x00", 127), (b"", 1)):
            with self.assertRaises(ValueError):
                video.unpack(data, pixels)

    def test_canvas_apply_turn_and_atomic_rejection(self):
        canvas = video.Canvas(2, 3)
        self.assertEqual(canvas.apply(present()), (1, 0, 1, 3, 2))
        rows = [b"\xff\x00\x00" + b"\0" * 3, b"\0" * 6, b"\0" * 6]
        self.assertEqual(canvas.frame(), b"".join(screenshot.turn_rgb_rows(rows, 2, 3)))
        self.assertEqual(canvas.frame(), b"\0" * 9 + b"\xff\0\0" + b"\0" * 6)
        old = canvas.frame()
        with self.assertRaises(ValueError):
            canvas.apply(present(pixel=b"\x07\xe0") + b"extra")
        self.assertEqual(canvas.frame(), old)

    def test_parts_rejoin_and_lost_out_of_order(self):
        stream = present()
        parts = video.Parts()
        self.assertIsNone(parts.feed(struct.pack("<IHB", 1, 0, 0) + stream[:10]))
        self.assertEqual(parts.feed(struct.pack("<IHB", 1, 1, 1) + stream[10:]), stream)
        self.assertIsNone(parts.feed(struct.pack("<IHB", 2, 0, 0) + b"abc"))
        self.assertIsNone(parts.feed(struct.pack("<IHB", 2, 2, 1) + b"def"))
        self.assertIsNone(parts.feed(struct.pack("<IHB", 2, 1, 1) + b"def"))
        self.assertEqual(parts.dropped, 1)
        self.assertEqual(parts.feed(struct.pack("<IHB", 3, 0, 1) + present(3)), present(3))
        self.assertIsNone(parts.feed(struct.pack("<IHB", 4, 1, 1) + present(4)))
        self.assertEqual(parts.dropped, 2)

    def test_resample_idle_and_burst(self):
        frames = []
        sample = video.Resample(frames.append)
        sample.present(100, b"a")
        sample.present(10_100, b"b")
        sample.present(20_100, b"c")
        sample.present(100_100, b"d")
        sample.finish()
        self.assertEqual(frames, [b"a", b"c", b"c", b"d"])

    def test_csv_and_encoder_options(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "run.mp4"
            process = mock.Mock()
            process.stdin = io.BytesIO()
            process.wait.return_value = 0
            with mock.patch.object(video.video_encode, "open_encoder", return_value=process) as popen:
                with video.Recording(out, log=io.StringIO(), binary="fake-ffmpeg") as recording:
                    with mock.patch.object(video.time, "time_ns", return_value=123):
                        recording.packet(struct.pack("<IHB", 1, 0, 1) + present())
                    recording.finish("PANELCAP_END presents=1 dropped=2 bytes=31")
                command = video.video_encode.command(*popen.call_args.args, **popen.call_args.kwargs)
                self.assertIn("libx264", command)
                self.assertIn("zerolatency", command)
                self.assertEqual(command[command.index("-g") + 1], "30")
            with out.with_suffix(".frames.csv").open() as source:
                rows = list(csv.reader(source))
            self.assertEqual(rows, [list(video.CSV_FIELDS), ["1", "0", "123", "1", "3", "2"]])

    def test_multiple_packet_tags_keep_single_tag_contract(self):
        import zlib
        def wire(tag, data):
            return f"{tag}_BIN {len(data)} {zlib.crc32(data):08x}\n".encode() + data
        data = b"hello " + wire("PANEL", b"pixels") + b"world\n" + wire("FRAMEREC", b"input")
        split = console_packets.Packets(("PANEL", "FRAMEREC"))
        result = []
        for byte in data:
            result.extend(split.feed(bytes([byte])))
        self.assertEqual(result, [("PANEL", b"pixels"), ("line", b"hello world"), ("FRAMEREC", b"input")])
        self.assertEqual(console_packets.Packets("PANEL").feed(wire("PANEL", b"x")), [("data", b"x")])

    def test_ffmpeg_pin_missing_patch_note_and_major_refusal(self):
        import video_encode
        with mock.patch.object(video_encode.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "ffmpeg is missing"):
                video_encode.resolve(io.StringIO())
        with mock.patch.object(video_encode, "pinned", return_value={"version": "6.1.1"}), \
                mock.patch.object(video_encode.shutil, "which", return_value="ffmpeg"):
            with mock.patch.object(video_encode, "version", return_value="6.1.2"):
                log = io.StringIO()
                self.assertEqual(video_encode.resolve(log), "ffmpeg")
                self.assertIn("6.1.2; pinned 6.1.1", log.getvalue())
            with mock.patch.object(video_encode, "version", return_value="7.0"):
                with self.assertRaisesRegex(RuntimeError, "7.0; pinned 6.1.1"):
                    video_encode.resolve(io.StringIO())


if __name__ == "__main__":
    unittest.main()
