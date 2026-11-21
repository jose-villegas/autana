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
    def capture(self, timed, corrupt=False, lost_first=False):
        good_seq = 2 if corrupt else 1
        stream = struct.pack("<IQHHHHHHI", good_seq, 0, 1, 0, 0, 0, 1, 1, 3) + b"\0\xf8\0"
        packet = struct.pack("<IHB", good_seq, 0, 1) + stream
        wire = b"PANEL_BIN %d %08x\n" % (len(packet), zlib.crc32(packet)) + packet

        if lost_first:
            first = struct.pack("<IHB", 1, 0, 0) + b"broken"
            tail = struct.pack("<IHB", 1, 1, 1) + b"tail"
            bad = b"PANEL_BIN %d %08x\n" % (len(first), zlib.crc32(first) ^ 1) + first
            damaged = bad + b"PANEL_BIN %d %08x\n" % (len(tail), zlib.crc32(tail)) + tail
        else:
            bad_packet = struct.pack("<IHB", 1, 0, 1) + struct.pack("<I", 1) + stream[4:]
            damaged = b"PANEL_BIN %d %08x\n" % (len(bad_packet), zlib.crc32(bad_packet)) + bad_packet[:-1] + b"\xff"

        class Board(RecordingBoard):
            def write(self, data):
                super().write(data)
                if data == b"PANELCAP STOP\n":
                    self.chunks.extend(([damaged] if corrupt else [])
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
                rows = list(csv.reader(source))
                self.assertEqual(len(rows), 2)
                self.assertEqual(rows[1][0], str(good_seq))

    def test_scripted_stdin_replies_and_recorded_packet_to_encoder(self):
        self.capture(False)

    def test_seconds_never_reads_stdin(self):
        self.capture(True)

    def test_bad_packet_drops_one_present_and_capture_continues(self):
        self.capture(False, corrupt=True)

    def test_first_part_crc_failure_counts_one_dropped_present(self):
        self.capture(False, corrupt=True, lost_first=True)


    def test_release_without_console_uses_records_bounded_end_wait(self):
        for command in ("record", "video"):
            with self.subTest(command=command), tempfile.TemporaryDirectory() as folder:
                args = Namespace(owner="agent", wait=0, out=str(Path(folder) / "run.mp4"), seconds=None, fps=15)
                board = RecordingBoard([], after_stop=[])
                store = mock_store()
                with mock.patch.object(device, "open_when_free", return_value=board), \
                        mock.patch.object(device, "RECORD_END_SECONDS", 0.05), \
                        mock.patch.object(device, "STREAM_CHECK_SECONDS", 0.01), \
                        mock.patch.object(panel_video.video_encode, "resolve", return_value="fake-ffmpeg"):
                    with self.assertRaisesRegex(RuntimeError, "no " + ("FRAMEREC" if command == "record" else "PANELCAP") + "_END"):
                        if command == "record":
                            device.record_frames(args, store, BOARD, lines=[], log=io.StringIO())
                        else:
                            device.video(args, store, BOARD, lines=[], log=io.StringIO())
                store.release.assert_called_once_with(BOARD, "token")
