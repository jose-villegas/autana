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


class PanelBoard(RecordingBoard):
    def write(self, data):
        super().write(data)
        if data == b"PANELCAP STOP\n":
            self.chunks.extend(self.after_stop)


class PanelVideoDeviceTests(unittest.TestCase):
    def capture(self, timed, corrupt=False, lost_first=False):
        good_seq = 2 if corrupt else 1
        stream = struct.pack("<IQHHHHHH", good_seq, 0, 1, 0, 0, 0, 1, 1) + b"\0\xf8\0"
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

        replies = ([damaged] if corrupt else []) + [wire, b"PANELCAP_END presents=1 dropped=0 bytes=27\n"]
        board = PanelBoard([b"PANELCAP_BEGIN fps=5 image=329728\n", b"DRAG_OK\n"], after_stop=replies)
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "run.mp4"
            args = Namespace(owner="agent", wait=0, out=out, fps=5, seconds=0.01 if timed else None)
            log = io.StringIO()
            process = mock.Mock()
            process.stdin = mock.Mock()
            process.wait.return_value = 0
            with mock.patch.object(device, "open_when_free", return_value=board), \
                    mock.patch.object(panel_video.video_encode, "resolve", return_value="fake-ffmpeg"), \
                    mock.patch.object(panel_video.video_encode, "open_encoder", return_value=process), \
                    mock.patch.object(device.sys, "stdin", mock.MagicMock(__iter__=mock.Mock(side_effect=AssertionError("stdin read"))) if timed else io.StringIO("DRAG 0 0 10 10 100\n")):
                self.assertEqual(device.video(args, mock_store(), BOARD, log=log), 0)
            self.assertEqual(board.writes[0], b"PANELCAP START 5\n")
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

    def test_same_read_delivers_valid_packets_around_damage(self):
        def wire(seq, us, part=0, last=1, damaged=False, payload=None):
            if payload is None:
                payload = panel_video.PRESENT.pack(seq, us, 0, 0)
            packet = panel_video.PART.pack(seq, part, last) + payload
            return b"PANEL_BIN %d %08x\n" % (len(packet), zlib.crc32(packet) ^ damaged) + packet

        replies = (
            wire(1, 0) + wire(2, 10, last=0) + wire(2, 10, part=1, damaged=True)
            + wire(3, 20, last=0) + wire(3, 20, part=2)
            + wire(4, 30, last=0) + wire(5, 100_000)
            + b"PANELCAP_END presents=5 dropped=0 bytes=0\n"
        )
        board = PanelBoard([b"PANELCAP_BEGIN fps=5 image=329728\n"], after_stop=[replies])
        store = mock_store()
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / "run.mp4"
            args = Namespace(owner="agent", wait=0, out=out, fps=5, seconds=None)
            log = io.StringIO()
            process = mock.Mock()
            process.wait.return_value = 0
            with mock.patch.object(device, "open_when_free", return_value=board), \
                    mock.patch.object(panel_video.video_encode, "resolve", return_value="fake-ffmpeg"), \
                    mock.patch.object(panel_video.video_encode, "open_encoder", return_value=process):
                self.assertEqual(device.video(args, store, BOARD, lines=[], log=log), 0)
            with out.with_suffix(".frames.csv").open() as source:
                self.assertEqual([row["present_seq"] for row in csv.DictReader(source)], ["1", "5"])
            self.assertEqual(process.stdin.write.call_count, 4)
            self.assertIn("host_dropped=3", log.getvalue())
        store.release.assert_called_once_with(BOARD, "token")

    def test_sink_failure_sends_stop_and_drains_to_end(self):
        def wire(seq, us):
            packet = panel_video.PART.pack(seq, 0, 1) + panel_video.PRESENT.pack(seq, us, 0, 0)
            return b"PANEL_BIN %d %08x\n" % (len(packet), zlib.crc32(packet)) + packet

        end = b"PANELCAP_END presents=3 dropped=0 bytes=48\n"
        board = PanelBoard([b"PANELCAP_BEGIN fps=5 image=329728\n", wire(1, 0) + wire(2, 100_000)],
                               after_stop=[wire(3, 200_000), end])
        store = mock_store()
        with tempfile.TemporaryDirectory() as folder:
            args = Namespace(owner="agent", wait=0, out=Path(folder) / "run.mp4", fps=5, seconds=10)
            log = io.StringIO()
            process = mock.Mock()
            process.stdin.write.side_effect = BrokenPipeError("sink failed")
            process.wait.return_value = 0
            with mock.patch.object(device, "open_when_free", return_value=board), \
                    mock.patch.object(panel_video.video_encode, "resolve", return_value="fake-ffmpeg"), \
                    mock.patch.object(panel_video.video_encode, "open_encoder", return_value=process):
                with self.assertRaisesRegex(BrokenPipeError, "sink failed"):
                    device.video(args, store, BOARD, log=log)
            self.assertEqual(board.writes, [b"PANELCAP START 5\n", b"PANELCAP STOP\n"])
            self.assertIn(end.decode().strip(), log.getvalue())
            process.wait.assert_called_once()
        store.release.assert_called_once_with(BOARD, "token")

    def test_short_app_memory_prints_refusal_and_exits_nonzero(self):
        line = panel_video.ERROR_PREFIX + "no memory: need 334064, largest free 210000"
        board = RecordingBoard([line.encode() + b"\n"], after_stop=[])
        with tempfile.TemporaryDirectory() as folder:
            args = Namespace(owner="agent", wait=0, out=Path(folder) / "run.mp4", fps=5, seconds=1)
            log = io.StringIO()
            with mock.patch.object(device, "open_when_free", return_value=board), \
                    mock.patch.object(device, "RECORD_END_SECONDS", 0.05), \
                    mock.patch.object(device, "STREAM_CHECK_SECONDS", 0.01), \
                    mock.patch.object(panel_video.video_encode, "resolve", return_value="fake-ffmpeg"):
                self.assertEqual(device.video(args, mock_store(), BOARD, lines=[], log=log), 1)
            self.assertEqual(board.writes, [b"PANELCAP START 5\n"])
            self.assertEqual(log.getvalue(), line + "\n")

    def test_release_without_console_uses_records_bounded_end_wait(self):
        for command in ("record", "video"):
            with self.subTest(command=command), tempfile.TemporaryDirectory() as folder:
                args = Namespace(owner="agent", wait=0, out=str(Path(folder) / "run.mp4"), seconds=None, fps=5)
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
