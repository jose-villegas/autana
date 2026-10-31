"""Capture host speech through ADC EQ arms inside one autana lock run."""
import json
import math
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace

import numpy as np
from scipy import signal
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from audio.mic.eq_probe_sweep import (ROOT, REGISTERS, DEFAULTS, BUTTERWORTH_Q, codec, program,
                                     restore, tones, capture_parser, capture_records, autana_run)
from audio.mic.eq_design import eq_design_biquad
from audio.mic import mic_metrics as metrics
from audio.mic.generate_noise import generate
from audio.sound_asset import SAMPLE_RATE_HZ

CAPTURE = ROOT / 'docs/audio/data/eq_voice/mic_eq_voice_capture.txt'
SPEECH = ROOT / 'docs/audio/data/speech'
SENTENCES = ('1462-170138-0007', '174-168635-0006')
RUNS = 3
PADDING_S = 1
NOISE_SNR_DB = 10
NOISE_SEED = 37
MATERIAL_PEAK = .8
SETTLE_MS = 500
LEVEL_MIN_DBFS = -50
HPF_CODE = 10
HIGH_PASS_HZ = 150
VOICE_LOW_HZ, VOICE_HIGH_HZ = metrics.SPEECH_LOW_HZ, metrics.SPEECH_HIGH_HZ
PRESENCE_HZ, PRESENCE_Q, PRESENCE_DB = 3000, 1, 6
SPEECH_HZ = (300, 500, 1000, 2000, 3000, 3400)
TONE_MS, TONE_GAIN = 500, 30
PLAYER_TIMEOUT_S = 30
HOST_PLAYER = Path(__file__).with_name('host_play.py')


def parser():
    result = capture_parser(__doc__, CAPTURE)
    result.add_argument('--runs', type=int, default=RUNS)
    result.add_argument('--host-gain', type=float, default=0)
    result.add_argument('--host-device')
    return result


def arms():
    center = math.sqrt(VOICE_LOW_HZ * VOICE_HIGH_HZ)
    return {'bypass': None,
            'highpass': eq_design_biquad('highpass', HIGH_PASS_HZ, BUTTERWORTH_Q),
            'voice': eq_design_biquad('bandpass', center, center / (VOICE_HIGH_HZ - VOICE_LOW_HZ)),
            'presence': eq_design_biquad('peaking', PRESENCE_HZ, PRESENCE_Q, gain_db=PRESENCE_DB)}


def material_samples(sentence, rate, seed=NOISE_SEED):
    clean = np.pad(sentence, (rate * PADDING_S, rate * PADDING_S))
    noise = generate('pink', len(clean) / rate, rate, seed)
    level = metrics.active_level_db(clean, rate)
    noise *= 10 ** ((level - NOISE_SNR_DB - metrics.noise_dbfs(noise)) / metrics.DB_AMPLITUDE)
    noisy = clean + noise
    gain = min(1, MATERIAL_PEAK / max(np.max(abs(clean)), np.max(abs(noisy))))
    return clean * gain, noisy * gain


def clean_sentence(sentence):
    if sentence not in SENTENCES:
        raise ValueError('unknown speech sentence')
    samples, rate = metrics.read_take(SPEECH / (sentence + '.flac'))
    divisor = math.gcd(rate, SAMPLE_RATE_HZ)
    return signal.resample_poly(samples, SAMPLE_RATE_HZ // divisor, rate // divisor)


def build_materials(directory):
    result = {}
    for sentence in SENTENCES:
        clean, noisy = material_samples(clean_sentence(sentence), SAMPLE_RATE_HZ)
        reference = directory / (sentence + '-quiet.wav')
        for condition, samples in (('quiet', clean), ('noise', noisy)):
            name = sentence + '-' + condition
            path = directory / (name + '.wav')
            sf.write(path, samples, SAMPLE_RATE_HZ, subtype='FLOAT')
            result[name] = dict(wav=path, reference=reference, sentence=sentence, condition=condition,
                                ms=math.ceil(len(samples) * metrics.MILLISECONDS / SAMPLE_RATE_HZ))
    return result


def speech_take(record, args, material, path):
    command = [sys.executable, str(HOST_PLAYER), str(material['wav']), '--gain', str(args.host_gain)]
    if args.host_device is not None:
        command += ['--device', args.host_device]
    record.note('host-play', dict(wav=material['wav'].name, gain=args.host_gain, device=args.host_device))
    player = subprocess.Popen(command)
    try:
        state = record('mic', 'record', material['ms'] + SETTLE_MS, '--settle', SETTLE_MS, '-o', str(path), '--json')
        if player.wait(timeout=PLAYER_TIMEOUT_S):
            raise RuntimeError('host playback failed; check --host-device and --host-gain')
        state = json.loads(state) if isinstance(state, str) else state
        path.with_suffix('.json').write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8', newline='\n')
    finally:
        if player.poll() is None:
            player.terminate()
            try:
                player.wait(timeout=PLAYER_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                player.kill()
                player.wait(timeout=PLAYER_TIMEOUT_S)


def level_check(path):
    samples, rate = metrics.read_take(path)
    if metrics.clip_count(samples):
        raise ValueError('bypass level check clips; lower --host-gain and retry')
    if metrics.active_level_db(samples, rate) < LEVEL_MIN_DBFS:
        raise ValueError(f'bypass level check below {LEVEL_MIN_DBFS} dBFS; raise --host-gain and retry')


def capture_arms(record, args, saved, materials):
    program(record, saved, 'dac', None)
    record('tune', 'audio.drc', 0)
    for name, value in dict(DEFAULTS, hpf=1, hpf_s1=HPF_CODE, hpf_s2=HPF_CODE).items():
        record('tune', 'mic.' + name, value)
    program(record, saved, 'adc', None)
    check_path = args.out.with_name('level-check.wav')
    record.note('level-check', {})
    speech_take(record, args, next(iter(materials.values())), check_path)
    level_check(check_path)
    record.note('runs', args.runs)
    record.note('materials', {name: {key: value for key, value in material.items() if key in ('sentence', 'condition')}
                              for name, material in materials.items()})
    for name, coefficients in arms().items():
        record.note('arm', dict(name=name, coefficients=coefficients))
        program(record, saved, 'adc', coefficients)
        for material_name, material in materials.items():
            record.note('material', material_name)
            for run in range(args.runs):
                speech_take(record, args, material, args.out.with_name(f'{name}-{material_name}-{run:02d}.wav'))
        tones(record, SimpleNamespace(runs=args.runs, ms=TONE_MS, volume=TONE_GAIN), SPEECH_HZ)
    record('buildid', '--json')


def capture(args):
    if args.runs <= 0 or not math.isfinite(args.host_gain):
        raise ValueError('positive runs and finite host gain required')
    with TemporaryDirectory(prefix='eq-voice-') as scratch:
        materials = build_materials(Path(scratch))
        with capture_records(args, 'ADC EQ voice capture',
                             lambda command, *words: autana_run(command, '--board', args.board, *words)) as record:
            saved, tunables = {}, {}
            try:
                record('status')
                values = json.loads(record('tune', '--json'))['tunables']
                available = {row['name']: row['value'] for row in values}
                tunables = {name: available[name] for name in (*('mic.' + key for key in DEFAULTS), 'audio.drc')}
                for reg in REGISTERS:
                    saved[reg] = codec(record, reg)
                capture_arms(record, args, saved, materials)
            finally:
                try:
                    record('sound', 'stop', 'all')
                finally:
                    if saved:
                        restore(record, saved, tunables)


def main():
    args = parser().parse_args()
    capture(args)
    print(f'ADC EQ voice capture: {args.out}')


if __name__ == '__main__':
    main()
