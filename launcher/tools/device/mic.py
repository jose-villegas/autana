"""Framed microphone PCM transport; the caller owns the serial port and lock."""
import base64
import json
from pathlib import Path
import sys
import time
import wave
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build.c_header import read_define
from audio.sound_asset import PCM_BYTES, CHANNELS
HEADER = Path(__file__).resolve().parents[2] / 'main/sound/console/console_mic_parse.h'
BEGIN, STATE, END, DATA, ERROR = [read_define(HEADER, 'MIC_RECORD_' + key)
                                  for key in ('BEGIN', 'STATE', 'END', 'DATA', 'ERROR')]
MILLISECONDS_PER_SECOND = read_define(Path(__file__).resolve().parents[2] / 'packages/math/include/math/scalar/units.h', 'MATH_MS_PER_S')
MAX_MS = read_define(HEADER, 'MIC_RECORD_MAX_MS')
DEFAULT_SETTLE_MS = read_define(Path(__file__).resolve().parents[2] / 'main/audio/audio_loopback.h', 'AUDIO_LOOPBACK_SETTLE_MS')
WRITES_MAX = read_define(HEADER, 'MIC_RECORD_WRITES_MAX')
TONES_MAX = read_define(HEADER, 'MIC_RECORD_TONES_MAX')
BYTE_MAX = read_define(HEADER, 'MIC_RECORD_BYTE_MAX')
OUT_HEADER = Path(__file__).resolve().parents[2] / 'main/audio/audio_out.h'
NYQUIST_HZ = read_define(OUT_HEADER, 'AUDIO_OUT_SAMPLE_RATE_HZ') // 2
VOLUME_MAX = read_define(OUT_HEADER, 'AUDIO_OUT_VOLUME_MAX_PCT')
TRANSFER_TIMEOUT_S = 120
SERIAL_READ_BYTES = 4096

class MicFrame:
    def __init__(self):
        self.begin = None
        self.state = None
        self.pcm = bytearray()

    def feed(self, line):
        if line.startswith(ERROR):
            raise RuntimeError(line)
        if line.startswith(BEGIN):
            if self.begin is not None:
                raise ValueError('duplicate MIC_BEGIN')
            self.begin = json.loads(line[len(BEGIN):])
            if self.begin['rate'] <= 0 or not 0 < self.begin['samples'] <= self.begin['rate'] * MAX_MS // MILLISECONDS_PER_SECOND:
                raise ValueError('invalid MIC_BEGIN size')
        elif self.begin is not None:
            if line.startswith(DATA):
                self.pcm.extend(base64.b64decode(line[len(DATA):], validate=True))
                if len(self.pcm) > self.begin['samples'] * PCM_BYTES:
                    raise ValueError('MIC payload exceeds announced size')
            elif line.startswith(STATE):
                if self.state is not None:
                    raise ValueError('duplicate MIC_STATE')
                self.state = json.loads(line[len(STATE):])
            elif line == END:
                if self.state is None or any(self.state[k] != self.begin[k] for k in ('rate', 'samples')):
                    raise ValueError('missing or mismatched MIC_STATE')
                if len(self.pcm) != self.begin['samples'] * PCM_BYTES:
                    raise ValueError('truncated MIC PCM')
                return bytes(self.pcm), self.state
        return None

def read_mic(port, ms, settle=None, timeout=TRANSFER_TIMEOUT_S, at=()):
    if not 0 < ms <= MAX_MS or (settle is not None and not 0 <= settle <= MAX_MS):
        raise ValueError('record and settle duration out of range')
    command = f'mic record {ms}' + (f' {settle}' if settle is not None else '')
    settle_ms = DEFAULT_SETTLE_MS if settle is None else settle
    writes = tones = 0
    for entry in at:
        if len(entry) not in (3, 5) or not -settle_ms <= entry[0] < ms:
            raise ValueError('invalid scheduled entry')
        if len(entry) == 5:
            when, kind, hz, duration, gain = entry
            if kind != 'tone' or not 0 < hz < NYQUIST_HZ or not 0 < duration <= ms - when or not 0 <= gain <= VOLUME_MAX:
                raise ValueError('scheduled tone out of range')
            tones += 1
        else:
            if any(not 0 <= value <= BYTE_MAX for value in entry[1:]):
                raise ValueError('scheduled codec write out of range')
            writes += 1
        command += ' at ' + ' '.join(map(str, entry))
    if writes > WRITES_MAX or tones > TONES_MAX:
        raise ValueError('too many scheduled entries')
    port.reset_input_buffer()
    port.write((command + '\n').encode('ascii'))
    frame = MicFrame()
    pending = b''
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pending += port.read(SERIAL_READ_BYTES)
        while b'\n' in pending:
            line, pending = pending.split(b'\n', 1)
            result = frame.feed(line.decode('ascii', errors='replace').rstrip('\r'))
            if result is not None:
                return result
    raise RuntimeError('timed out waiting for MIC_END')

def write_take(out, pcm, state):
    out = Path(out).with_suffix('.wav')
    out.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(out), 'wb') as wav:
        wav.setparams((CHANNELS, PCM_BYTES, state['rate'], 0, 'NONE', ''))
        wav.writeframes(pcm)
    out.with_suffix('.json').write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
    return out
