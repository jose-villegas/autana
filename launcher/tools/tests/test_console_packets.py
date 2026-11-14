"""launcher/tools/device/console_packets.py: binary packets split from a board's text lines.

    python -m unittest discover -s launcher/tools/tests

Packets and lines come out in order however the reads cut them; a packet
that lands inside another task's line leaves that line whole; text that
only looks like a packet head inside a line stays text; what a stream cut
short leaves is the cut line and the packet's bytes so far; a damaged
packet is refused.
"""
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/device"))

import console_packets  # noqa: E402

TAG = "SAMPLETAG"


def packet(data):
    return b"SAMPLETAG_BIN %d %08x\n" % (len(data), zlib.crc32(data)) + data


def split(wire, cut):
    packets = console_packets.Packets(TAG)
    events = packets.feed(wire[:cut]) + packets.feed(wire[cut:])
    return events, packets.rest()


class PacketTests(unittest.TestCase):
    def test_packets_and_lines_come_out_in_order_however_the_reads_cut_them(self):
        wire = b"I (1) app: hello\n" + packet(b"\x00\n\x01SAMPLETAG") + b"SAMPLETAG_END frames=1\n" + packet(b"x" * 9)
        for cut in range(1, len(wire)):
            with self.subTest(cut=cut):
                self.assertEqual(split(wire, cut), ([("line", b"I (1) app: hello"), ("data", b"\x00\n\x01SAMPLETAG"),
                                                     ("line", b"SAMPLETAG_END frames=1"), ("data", b"x" * 9)], b""))

    def test_a_packet_inside_a_line_written_in_parts_leaves_the_line_whole(self):
        # console_emit_line() writes prefix, payload and newline apart: a packet can land between them.
        wire = b"SAMPLETAG_" + packet(b"\x00\n\x01") + b"END frames=1\n" + packet(b"abc")
        for cut in range(1, len(wire)):
            with self.subTest(cut=cut):
                self.assertEqual(split(wire, cut), ([("data", b"\x00\n\x01"), ("line", b"SAMPLETAG_END frames=1"),
                                                     ("data", b"abc")], b""))

    def test_text_inside_a_line_that_only_looks_like_a_head_stays_text(self):
        wire = b"W (5) log: saw SAMPLETAG_BIN twice\n" + packet(b"z")
        self.assertEqual(split(wire, len(wire)),
                         ([("line", b"W (5) log: saw SAMPLETAG_BIN twice"), ("data", b"z")], b""))

    def test_two_packets_inside_one_line_both_come_out_and_the_line_is_whole(self):
        wire = b"I (9) a" + packet(b"one") + b"pp: hel" + packet(b"") + b"lo\r\n"
        for cut in range(1, len(wire)):
            with self.subTest(cut=cut):
                self.assertEqual(split(wire, cut),
                                 ([("data", b"one"), ("data", b""), ("line", b"I (9) app: hello")], b""))

    def test_what_is_left_is_the_cut_line_and_a_packet_cut_short_is_not_data(self):
        wire = b"I (1) cut" + packet(b"whole") + b" off" + packet(b"short")[:-2]
        events, rest = split(wire, len(wire))
        self.assertEqual(events, [("data", b"whole")])
        self.assertEqual(rest, b"I (1) cut off" + packet(b"short")[:-2])

    def test_a_packet_waits_for_all_its_bytes(self):
        packets = console_packets.Packets(TAG)
        self.assertEqual(packets.feed(packet(b"abcdef")[:-1]), [])
        self.assertEqual(packets.feed(b"f"), [("data", b"abcdef")])

    def test_another_tag_s_packet_is_a_line(self):
        wire = b"SHOT_BIN 1 00000000\n"
        self.assertEqual(split(wire, len(wire)), ([("line", b"SHOT_BIN 1 00000000")], b""))

    def test_a_damaged_packet_is_refused(self):
        damaged = bytearray(packet(b"abcdef"))
        damaged[-1] ^= 1
        with self.assertRaisesRegex(console_packets.PacketError, "damaged"):
            console_packets.Packets(TAG).feed(bytes(damaged))

    def test_a_head_opening_a_line_that_does_not_parse_is_refused(self):
        with self.assertRaisesRegex(console_packets.PacketError, "not a packet line"):
            console_packets.Packets(TAG).feed(b"SAMPLETAG_BIN twelve\n")


if __name__ == "__main__":
    unittest.main()
