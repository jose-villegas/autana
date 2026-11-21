"""Panel video uses record's stdin, console reply, STOP and lock contract."""
import csv
import io
import struct
import tempfile
from argparse import Namespace
from pathlib import Path
from unittest import mock
import unittest
import zlib

from test_device import device, RecordingBoard, mock_store, BOARD
import panel_video


class PanelVideoDeviceTests(unittest.TestCase):
    def capture(self, timed, corrupt=False):
        stream = struct.pack("<IQHHHHHHI", 1, 0, 1, 0, 0, 0, 1, 1, 3) + b"\0\xf8\0"
        packet = struct.pack("<IHB", 1, 0, 1) + stream
        wire = b"PANEL_BIN %d %08x\n" % (len(packet), zlib.crc32(packet)) + packet

        class Board(RecordingBoard):
            def write(self, data):
                super().write(data)
                if data == b"PANELCAP STOP\n":
                    self.chunks.extend(([wire[:-1] + b"\xff"] if corrupt else [])
                                       + [wire, b"PANELCAP_END presents=1 dropped=0 bytes=31\n"])

        board = Board([b"PANELCAP_BEGIN fps=15 ring=1331232\n", b"DRAG_OK\n"])
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "run.mp4"
            args = Namespace(owner="agent", wait=0, out=out, fps=15, seconds=0.01 if timed else None)
            log = io.StringIO()
            process = mock.Mock()
            process.stdin = mock.Mock()
            process.wait.return_value = 0
            with mock.patch.object(device, "open_when_free", return_value=board), \
                    mock.patch.object(panel_video.video_encode, "resolve", return_value="fake-ffmpeg"), \
                    mock.patch.object(panel_video.video_encode, "open_encoder", return_value=process), \
                    mock.patch.object(device.sys, "stdin", mock.MagicMock(__iter__=mock.Mock(side_effect=AssertionError("stdin read"))) if timed else io.StringIO("DRAG 0 0 10 10 100\n")):
                self.assertEqual(device.video(args, mock_store(), BOARD, log=log), 0)
            self.assertEqual(board.writes[0], b"PANELCAP START 15\n")
            self.assertEqual(board.writes[-1], b"PANELCAP STOP\n")
            if not timed:
                self.assertIn(b"DRAG 0 0 10 10 100\n", board.writes)
            self.assertTrue(process.stdin.write.called)
            frame = process.stdin.write.call_args_list[0].args[0]
            self.assertEqual(len(frame), panel_video.screenshot.PANEL_WIDTH * panel_video.screenshot.PANEL_HEIGHT * 3)
            self.assertEqual(frame.count(b"\xff"), 1)
            self.assertEqual(set(frame), {0, 255})
            self.assertIn("DRAG_OK", log.getvalue())
            self.assertIn("video: presents=1", log.getvalue())
            self.assertIn(f"host_dropped={int(corrupt)}", log.getvalue())
            with out.with_suffix(".frames.csv").open() as source:
                self.assertEqual(len(list(csv.reader(source))), 2)

    def test_scripted_stdin_replies_and_recorded_packet_to_encoder(self):
        self.capture(False)

    def test_seconds_never_reads_stdin(self):
        self.capture(True)

    def test_bad_packet_drops_one_present_and_capture_continues(self):
        self.capture(False, corrupt=True)
