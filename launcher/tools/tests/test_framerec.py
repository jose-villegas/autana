"""launcher/tools/device/framerec.py: the record reader.

    python -m unittest discover -s launcher/tools/tests

Records walked by their head, a type the reader does not know passed
through, a record cut short or of the wrong size refused; and the reader's
type numbers and flags held to frame_record_format.h. The record
sizes are held to the firmware by a real recording (the render lab's
tools/tests/test_record_host.py), whose every fixed-size record the reader
measures.
"""
import re
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/device"))
sys.path.insert(0, str(ROOT / "launcher/tools"))

import framerec  # noqa: E402
from build.c_header import read_define  # noqa: E402

FORMAT_HEADER = ROOT / "launcher/main/render/record/frame_record_format.h"


def record(name, body):
    return framerec.HEAD.pack(framerec.RECORD_TYPES[name] if isinstance(name, str) else name, len(body)) + body


def header(keyframe_every=30):
    return record("HEADER", framerec.HEADER.pack(framerec.VERSION, 368, 448, keyframe_every, b"host"))


def frame(index, flags=0):
    return record("FRAME", framerec.FRAME.pack(index, 16, flags, 0, 0, 0))


class FormatTests(unittest.TestCase):
    def test_the_type_numbers_and_flags_are_the_firmware_header_s(self):
        text = FORMAT_HEADER.read_text(encoding="utf-8")
        for table, enum in ((framerec.RECORD_TYPES, "frame_record_type_t"), (framerec.MESH_KINDS, "frame_record_kind_t")):
            with self.subTest(enum=enum):
                body = re.search(r"typedef enum \{([^}]*)\} " + enum, text)[1]
                self.assertEqual(table, {name: int(n) for name, n in re.findall(r"FRAME_RECORD_(\w+) = (\d+),", body)})
        for name in ("KEYFRAME", "AFTER_DROP", "GENERATED"):
            with self.subTest(flag=name):
                self.assertEqual(getattr(framerec, name), read_define(FORMAT_HEADER, "FRAME_RECORD_" + name))

        self.assertEqual(framerec.MESH.size, read_define(FORMAT_HEADER, "FRAME_RECORD_MESH_BYTES"))


class RecordTests(unittest.TestCase):
    def test_a_stream_reads_back_into_its_header_frames_and_checks(self):
        check = record("CHECK", framerec.CHECK.pack(0, 4, 2, 0xC0FFEE))
        tune = record("TUNE", framerec.TUNE.pack(b"render.cull", 1))
        stream = framerec.parse(header() + frame(0, framerec.KEYFRAME) + frame(1) + check + tune)
        self.assertEqual(stream.header["keyframe_every"], 30)
        self.assertEqual([f.index for f in stream.frames], [0, 1])
        self.assertEqual([name for name, _ in stream.frames[1].records], ["CHECK", "TUNE"])
        self.assertEqual(stream.checks, {0: {"width": 4, "height": 2, "crc32": 0xC0FFEE}})
        self.assertEqual(framerec.decode_tune(stream.frames[1].records[1][1]), ("render.cull", 1))
        self.assertEqual(framerec.summary(stream)["keyframes"], 1)

    def test_a_type_the_reader_does_not_know_passes_by_its_number(self):
        stream = framerec.parse(header() + frame(0) + record(200, b"later"))
        self.assertEqual(stream.frames[0].records, [(200, b"later")])

    def test_truncated_frame_is_not_counted(self):
        incomplete = record("FRAME", framerec.FRAME.pack(1, 16, 0, 0, 0, 100))
        stream = framerec.parse(header() + frame(0) + incomplete)
        self.assertEqual([f.index for f in stream.frames], [0])

    def test_record_cannot_cross_declared_frame_boundary(self):
        payload = record(200, b"later")
        framed = record("FRAME", framerec.FRAME.pack(0, 16, 0, 0, 0, len(payload) - 1))
        with self.assertRaisesRegex(framerec.StreamError, "frame"):
            framerec.parse(header() + framed + payload)

    def test_a_stream_that_does_not_open_with_its_header_is_refused(self):
        with self.assertRaisesRegex(framerec.StreamError, "opens with HEADER"):
            framerec.parse(frame(0))

    def test_a_stream_of_another_version_is_refused(self):
        older = record("HEADER", framerec.HEADER.pack(framerec.VERSION - 1, 368, 448, 30, b"host"))
        with self.assertRaisesRegex(framerec.StreamError, "frame record version"):
            framerec.parse(older + frame(0))

    def test_a_record_cut_short_is_refused(self):
        with self.assertRaisesRegex(framerec.StreamError, "cut short"):
            framerec.parse(header() + frame(0)[:-1])

    def test_a_fixed_record_of_the_wrong_size_is_refused(self):
        with self.assertRaisesRegex(framerec.StreamError, "the format's is"):
            framerec.parse(header() + record("FRAME", struct.pack("<I", 0)))

    def test_a_mesh_lit_per_face_reads_back_with_its_face_colours_where_they_are(self):
        positions, colours, batch = (1, 10, 100), (1, 11, 200), (2, 12, 300)
        body = framerec.MESH.pack(3, framerec.MESH_KINDS["STATIC"], True, True, *positions, *colours, *batch,
                                  *[0.0] * 16)
        mesh = framerec.decode_mesh(body)
        self.assertTrue(mesh["faces"])
        self.assertEqual((mesh["positions"], mesh["colours"], mesh["batch"]), (positions, colours, batch))


if __name__ == "__main__":
    unittest.main()
