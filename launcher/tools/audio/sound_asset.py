"""Encode, validate and read NAME.sound.toml and SOND entries.

The TOML owns format and loop frames at the stored rate. Layout:
docs/assets/formats/SOND.md. Standard library only; sound_import.py decodes sources.
"""

import pathlib
import struct
import sys
import tomllib

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from build.c_header import read_define

TYPE = b"SOND"
SUFFIX = ".sound.toml"
SOUND_HEADER = TOOLS.parent / "main/sound/sound_format.h"
ADPCM_HEADER = TOOLS.parent / "main/audio/audio_adpcm.h"


VERSION = read_define(SOUND_HEADER, "SOUND_VERSION")
HEADER_SIZE = read_define(SOUND_HEADER, "SOUND_HEADER_SIZE")
ALIGNMENT = read_define(SOUND_HEADER, "SOUND_ALIGNMENT")
SAMPLE_RATE_HZ = read_define(TOOLS.parent / "main/audio/audio_out.h", "AUDIO_OUT_SAMPLE_RATE_HZ")
CHANNELS = read_define(SOUND_HEADER, "SOUND_CHANNELS")
DEFAULT_BLOCK_FRAMES = read_define(SOUND_HEADER, "SOUND_DEFAULT_BLOCK_FRAMES")
PCM_BYTES = read_define(SOUND_HEADER, "SOUND_PCM_BYTES")
AT_VERSION, AT_FORMAT, AT_CHANNELS, AT_SAMPLE_RATE, AT_FRAMES, AT_LOOP_START, AT_LOOP_END, AT_BLOCK_FRAMES, \
    AT_RESERVED16, AT_RESERVED32, AT_DATA = [read_define(SOUND_HEADER, "SOUND_AT_" + name) for name in
        ("VERSION", "FORMAT", "CHANNELS", "SAMPLE_RATE", "FRAMES", "LOOP_START", "LOOP_END", "BLOCK_FRAMES",
         "RESERVED16", "RESERVED32", "DATA")]
BLOCK_HEADER_BYTES = read_define(ADPCM_HEADER, "AUDIO_ADPCM_BLOCK_HEADER_BYTES")
STEP_INDEX_MAX = read_define(ADPCM_HEADER, "AUDIO_ADPCM_STEP_INDEX_MAX")
ADPCM_AT_STEP_INDEX = read_define(ADPCM_HEADER, "AUDIO_ADPCM_AT_STEP_INDEX")
ADPCM_AT_RESERVED = read_define(ADPCM_HEADER, "AUDIO_ADPCM_AT_RESERVED")
CODES_PER_BYTE = read_define(ADPCM_HEADER, "AUDIO_ADPCM_CODES_PER_BYTE")
CODE_BITS = read_define(ADPCM_HEADER, "AUDIO_ADPCM_CODE_BITS")
CODE_MASK = read_define(ADPCM_HEADER, "AUDIO_ADPCM_CODE_MASK")
SIGN_BIT = read_define(ADPCM_HEADER, "AUDIO_ADPCM_SIGN_BIT")
STEPS = read_define(ADPCM_HEADER, "AUDIO_ADPCM_STEP_TABLE")
INDEX = read_define(ADPCM_HEADER, "AUDIO_ADPCM_INDEX_TABLE")
FORMATS = {"pcm16": read_define(SOUND_HEADER, "SOUND_FORMAT_PCM16"), "adpcm": read_define(SOUND_HEADER, "SOUND_FORMAT_IMA_ADPCM")}
SAMPLE_BITS = PCM_BYTES * 8
SAMPLE_MAX = (1 << (SAMPLE_BITS - 1)) - 1
SAMPLE_MIN = -(1 << (SAMPLE_BITS - 1))
FRAME_MAX = (1 << 32) - 1
SOURCE_SUFFIXES = (".wav", ".flac", ".ogg", ".mp3")


class SoundError(ValueError):
    """A source or entry that cannot represent a stored sound."""


def block_bytes(frames):
    return BLOCK_HEADER_BYTES + (frames - 1) // CODES_PER_BYTE


def clamp(value):
    return max(SAMPLE_MIN, min(SAMPLE_MAX, value))


def advance(predictor, index, code):
    step = STEPS[index]
    diff = step >> 3
    if code & 1:
        diff += step >> 2
    if code & 2:
        diff += step >> 1
    if code & 4:
        diff += step
    return clamp(predictor + (-diff if code & SIGN_BIT else diff)), max(0, min(STEP_INDEX_MAX, index + INDEX[code]))


def encode_adpcm(samples, frames):
    result = bytearray()
    index = 0
    for first in range(0, len(samples), frames):
        chunk = samples[first:first + frames]
        chunk += [chunk[-1]] * (frames - len(chunk))
        predictor = chunk[0]
        result += struct.pack("<hBB", predictor, index, 0)
        codes = []
        for sample in chunk[1:]:
            delta = sample - predictor
            code = SIGN_BIT if delta < 0 else 0
            delta = abs(delta)
            step = STEPS[index]
            for bit, term in ((4, step), (2, step >> 1), (1, step >> 2)):
                if delta >= term:
                    code |= bit
                    delta -= term
            predictor, index = advance(predictor, index, code)
            codes.append(code)
        result += bytes(codes[i] | codes[i + 1] << CODE_BITS for i in range(0, len(codes), CODES_PER_BYTE))
    return result


def validate_loop(loop, frames, block_frames):
    if loop is None:
        return 0, 0
    if not isinstance(loop, (list, tuple)) or len(loop) != 2 or any(type(v) is not int for v in loop):
        raise SoundError("loop must be [start, end] in stored frames")
    start, end = loop
    if not 0 <= start < end <= frames:
        raise SoundError("loop requires 0 <= start < end <= frame_count")
    if block_frames and start % block_frames:
        before = start // block_frames * block_frames
        raise SoundError(f"loop_start {start} is not block-aligned; nearest valid starts: {before} and {before + block_frames}")
    return start, end


def encode(samples, fmt, loop=None):
    samples = list(samples)
    if fmt not in FORMATS:
        raise SoundError("format must be adpcm or pcm16")
    if not 0 < len(samples) <= FRAME_MAX or any(type(v) is not int or not SAMPLE_MIN <= v <= SAMPLE_MAX for v in samples):
        raise SoundError("samples must be a nonempty sequence of int16 values")
    block = DEFAULT_BLOCK_FRAMES if fmt == "adpcm" else 0
    start, end = validate_loop(loop, len(samples), block)
    header = bytearray(HEADER_SIZE)
    for at, encoding, value in ((AT_VERSION, "H", VERSION), (AT_FORMAT, "B", FORMATS[fmt]),
            (AT_CHANNELS, "B", CHANNELS), (AT_SAMPLE_RATE, "I", SAMPLE_RATE_HZ), (AT_FRAMES, "I", len(samples)),
            (AT_LOOP_START, "I", start), (AT_LOOP_END, "I", end), (AT_BLOCK_FRAMES, "H", block),
            (AT_DATA, "I", HEADER_SIZE)):
        struct.pack_into("<" + encoding, header, at, value)
    data = encode_adpcm(samples, block) if block else struct.pack(f"<{len(samples)}h", *samples)
    return bytes(header + data)


def decode(entry):
    if len(entry) < HEADER_SIZE:
        raise SoundError("entry is shorter than its header")
    def field(at, encoding="I"):
        return struct.unpack_from("<" + encoding, entry, at)[0]
    version, fmt = field(AT_VERSION, "H"), field(AT_FORMAT, "B")
    frames, start, end = field(AT_FRAMES), field(AT_LOOP_START), field(AT_LOOP_END)
    block, offset = field(AT_BLOCK_FRAMES, "H"), field(AT_DATA)
    if version != VERSION or fmt not in FORMATS.values() or field(AT_CHANNELS, "B") != CHANNELS \
            or field(AT_SAMPLE_RATE) != SAMPLE_RATE_HZ or not frames or any(entry[AT_RESERVED16:AT_DATA]):
        raise SoundError("invalid sound header")
    fmt = next(name for name, value in FORMATS.items() if value == fmt)
    if (fmt == "pcm16" and block != 0) or (fmt == "adpcm" and (block == 0 or block % 2 == 0)):
        raise SoundError("invalid block_frames")
    if end == 0:
        if start != 0:
            raise SoundError("one-shot loop_start must be zero")
    else:
        validate_loop([start, end], frames, block)
    count = (frames + block - 1) // block if block else 0
    stride = block_bytes(block) if block else 0
    needed = count * stride if block else frames * PCM_BYTES
    if offset < HEADER_SIZE or offset % ALIGNMENT or offset + needed > len(entry):
        raise SoundError("sound data leaves the entry or is misaligned")
    if any(entry[HEADER_SIZE:offset]):
        raise SoundError("nonzero header gap")
    if not block:
        return fmt, list(struct.unpack_from(f"<{frames}h", entry, offset)), start, end, block
    samples = []
    for i in range(count):
        at = offset + i * stride
        predictor = field(at, "h")
        index = entry[at + ADPCM_AT_STEP_INDEX]
        if index > STEP_INDEX_MAX or entry[at + ADPCM_AT_RESERVED]:
            raise SoundError("invalid ADPCM block header")
        samples.append(predictor)
        for j in range(min(block, frames - i * block) - 1):
            code = (entry[at + BLOCK_HEADER_BYTES + j // CODES_PER_BYTE] >> (j % CODES_PER_BYTE * CODE_BITS)) & CODE_MASK
            predictor, index = advance(predictor, index, code)
            samples.append(predictor)
    return fmt, samples, start, end, block


def load_source(path):
    path = pathlib.Path(path)
    try:
        values = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError) as error:
        raise SoundError(f"{path}: {error}") from error
    source, fmt, loop = values.get("source"), values.get("format"), values.get("loop")
    if set(values) - {"source", "format", "loop"} or not isinstance(source, str) \
            or not isinstance(fmt, str) or fmt not in FORMATS:
        raise SoundError(f"{path}: requires source and format = adpcm or pcm16; allowed keys: source, format, loop")
    if pathlib.PurePath(source).name != source or ":" in source or "\\" in source or pathlib.Path(source).suffix.lower() not in SOURCE_SUFFIXES:
        raise SoundError(f"{path}: source {source!r} is not a WAV, FLAC, OGG or MP3 in the same folder")
    if "loop" in values:
        validate_loop(loop, FRAME_MAX, 0)
    return path.parent / source, fmt, loop


def sound_id(path):
    return pathlib.Path(path).name.removesuffix(SUFFIX)
