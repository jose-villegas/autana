#!/usr/bin/env python3
"""Verify a render_video.c AVI: header, index and file size all agree.

    python launcher/tools/render/check_avi.py <file.avi>...

Re-parses the RIFF structure from scratch - walking LIST/chunk headers
rather than trusting the byte offsets render_video.c itself used - and
checks that the frame count, frame size, rate and idx1 entries it finds are
mutually consistent, and that the RIFF/LIST size fields match what is
actually there. Exits nonzero on the first mismatch. Standard library only.
"""
import struct
import sys


class AviError(Exception):
    pass


def read_chunks(data, start, end):
    """Yields (fourcc, chunk_data_start, chunk_data_end) for each top-level
    chunk between start and end. A LIST's own 4-byte list type is left in
    its data, matching how a caller distinguishes LIST 'hdrl' from LIST
    'movi'."""
    pos = start
    while pos + 8 <= end:
        fourcc = data[pos : pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body_start = pos + 8
        body_end = body_start + size
        if body_end > end:
            raise AviError(f"chunk {fourcc!r} at {pos} claims size {size}, past its container's end")
        yield fourcc, body_start, body_end
        pos = body_end + (size & 1)  # RIFF pads odd-sized chunks to a word


def find_chunk(data, start, end, fourcc):
    for cc, s, e in read_chunks(data, start, end):
        if cc == fourcc:
            return s, e
    raise AviError(f"no {fourcc!r} chunk found")


def find_list(data, start, end, list_type):
    for cc, s, e in read_chunks(data, start, end):
        if cc == b"LIST" and data[s : s + 4] == list_type:
            return s + 4, e
    raise AviError(f"no LIST {list_type!r} found")


def check_file(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[0:4] != b"RIFF" or data[8:12] != b"AVI ":
        raise AviError("not a RIFF AVI file")
    riff_size = struct.unpack_from("<I", data, 4)[0]
    if riff_size + 8 != len(data):
        raise AviError(f"RIFF size {riff_size} + 8 != file size {len(data)}")

    body_start, body_end = 12, len(data)

    hdrl_s, hdrl_e = find_list(data, body_start, body_end, b"hdrl")
    avih_s, avih_e = find_chunk(data, hdrl_s, hdrl_e, b"avih")
    (us_per_frame, _max_bps, _pad, _flags, total_frames, _init, streams, _sugg, width, height) = struct.unpack_from(
        "<10I", data, avih_s
    )
    if streams != 1:
        raise AviError(f"avih declares {streams} streams, expected 1")

    strl_s, strl_e = find_list(data, hdrl_s, hdrl_e, b"strl")
    strh_s, strh_e = find_chunk(data, strl_s, strl_e, b"strh")
    fcc_type, fcc_handler = data[strh_s : strh_s + 4], data[strh_s + 4 : strh_s + 8]
    (dw_scale, dw_rate, _start, length, sugg_buf) = struct.unpack_from("<5I", data, strh_s + 20)
    if fcc_type != b"vids":
        raise AviError(f"strh fccType is {fcc_type!r}, expected 'vids'")

    strf_s, strf_e = find_chunk(data, strl_s, strl_e, b"strf")
    (bi_size, bi_width, bi_height, bi_planes, bi_bitcount, bi_compression, bi_size_image) = struct.unpack_from(
        "<IiiHHII", data, strf_s
    )
    if bi_size != 40 or bi_planes != 1 or bi_bitcount != 24 or bi_compression != 0:
        raise AviError(f"strf is not a plain 24bpp BI_RGB BITMAPINFOHEADER: {bi_size},{bi_planes},{bi_bitcount},{bi_compression}")
    if bi_width != width or bi_height != height:
        raise AviError(f"strf {bi_width}x{bi_height} disagrees with avih {width}x{height}")

    movi_data_s, movi_e = find_list(data, body_start, body_end, b"movi")
    movi_fourcc_pos = movi_data_s - 4  # idx1 offsets are relative to here

    frame_stride = ((width * 3 + 3) // 4) * 4
    expected_frame_bytes = frame_stride * height
    if sugg_buf != expected_frame_bytes:
        raise AviError(f"strh dwSuggestedBufferSize {sugg_buf} != computed frame size {expected_frame_bytes}")
    if bi_size_image != expected_frame_bytes:
        raise AviError(f"strf biSizeImage {bi_size_image} != computed frame size {expected_frame_bytes}")

    movi_frames = []
    for cc, s, e in read_chunks(data, movi_data_s, movi_e):
        if cc != b"00dc":
            raise AviError(f"unexpected chunk {cc!r} inside movi")
        if e - s != expected_frame_bytes:
            raise AviError(f"movi frame {len(movi_frames)} is {e - s} bytes, expected {expected_frame_bytes}")
        movi_frames.append(s - 8)  # position of the chunk's own fourcc

    idx1_s, idx1_e = find_chunk(data, movi_e, body_end, b"idx1")
    if (idx1_e - idx1_s) % 16 != 0:
        raise AviError(f"idx1 size {idx1_e - idx1_s} is not a multiple of 16")
    idx_entries = (idx1_e - idx1_s) // 16
    for i in range(idx_entries):
        entry = data[idx1_s + i * 16 : idx1_s + (i + 1) * 16]
        ck_id, _flags, offset, size = struct.unpack("<4sIII", entry)
        if ck_id != b"00dc":
            raise AviError(f"idx1 entry {i} names {ck_id!r}, expected '00dc'")
        if size != expected_frame_bytes:
            raise AviError(f"idx1 entry {i} size {size} != frame size {expected_frame_bytes}")

    counts = {"avih.dwTotalFrames": total_frames, "strh.dwLength": length, "movi frames": len(movi_frames), "idx1 entries": idx_entries}
    if len(set(counts.values())) != 1:
        raise AviError(f"frame counts disagree: {counts}")

    if idx_entries != len(movi_frames):
        raise AviError(f"idx1 has {idx_entries} entries but movi has {len(movi_frames)} frames")
    for i, movi_pos in enumerate(movi_frames):
        entry = data[idx1_s + i * 16 : idx1_s + (i + 1) * 16]
        _ck_id, _flags, offset, _size = struct.unpack("<4sIII", entry)
        if movi_fourcc_pos + offset != movi_pos:
            raise AviError(f"idx1 entry {i} offset {offset} does not point at movi frame {i} (at {movi_pos - movi_fourcc_pos})")

    if dw_scale == 0:
        raise AviError("strh dwScale is 0")
    fps = dw_rate / dw_scale
    if us_per_frame == 0:
        raise AviError("avih dwMicroSecPerFrame is 0")
    avih_fps = 1_000_000 / us_per_frame
    if abs(fps - avih_fps) > 1e-6:
        raise AviError(f"strh rate {fps} fps disagrees with avih {avih_fps} fps")

    print(
        f"{path}: ok, {total_frames} frames, {width}x{height}, {fps:g} fps "
        f"(dwRate/dwScale {dw_rate}/{dw_scale}), {len(data)} bytes"
    )


def main(argv):
    if not argv:
        print("usage: check_avi.py <file.avi>...", file=sys.stderr)
        return 2
    failed = False
    for path in argv:
        try:
            check_file(path)
        except (AviError, struct.error, OSError) as exc:
            print(f"{path}: FAIL: {exc}", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
