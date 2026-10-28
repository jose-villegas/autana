"""Capture playback DSP arms with standard-library-only board commands.

autana --wait 3600 lock run -- python launcher/tools/audio/mic/playback_dsp_capture.py --board <id>
"""
from pathlib import Path
import sys
import argparse

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from audio.mic.eq_probe_sweep import (ROOT, REGISTERS, CSM_REG, codec, program, restart, restore,
                            capture_parser, capture_records, autana_run, read_define, DEFAULTS)
from audio.mic.eq_design import eq_design_first_order_highpass
from autana_runner import reset_tunables

BYTE_MAX = read_define(ROOT / 'launcher/main/sound/console/console_mic_parse.h', 'MIC_RECORD_BYTE_MAX')
CAPTURE = ROOT / 'docs/audio/data/playback/mic_playback_capture.txt'
# Development builds ship this public-domain excerpt in the console pack (docs/audio/Sound-Assets.md).
MUSIC = 'console/carreno_waltz'
TAKE_MS = 3000
SETTLE_MS = 500
TONE_MS = 1000
TONE_HZ = 1000
LOW_TONE_HZ = 200
GAIN = 100
RUNS = 3
VOLUMES = (50, 70, 100)
EQ_VOLUMES = (70, 100)
CUTOFFS = (200, 400)
RAMPS = (0, 2, 6, 10)
STEP_AT_MS = (1000, 2000)
STEP_DB = 20
REGISTER_DB_PER_STEP = 0.5
REGISTER_STEP = round(STEP_DB / REGISTER_DB_PER_STEP)
DAC_VOLUME_REG = read_define(ROOT / 'launcher/main/audio/audio_codec_drc.h', 'AUDIO_CODEC_DAC_VOLUME_REG')
ADC_VOLUME_REG = read_define(ROOT / 'launcher/main/audio/audio_codec_saved.h', 'AUDIO_CODEC_MIC_VOLUME_REG')
ADC_RAMP_REG = read_define(ROOT / 'launcher/main/audio/audio_codec_mic.h', 'AUDIO_CODEC_MIC_RAMP_REG')
RAMP_SHIFT = read_define(ROOT / 'launcher/main/audio/audio_codec_mic.h', 'AUDIO_CODEC_MIC_NIBBLE_SHIFT')
RAMP_MASK = read_define(ROOT / 'launcher/main/audio/audio_codec_mic.h', 'AUDIO_CODEC_MIC_NIBBLE_MAX') << RAMP_SHIFT
AUDIO_TUNABLES = ('codec_volume', 'drc', 'drc_ramp')


def parser():
    result = capture_parser(__doc__, CAPTURE)
    result.formatter_class = argparse.RawDescriptionHelpFormatter
    result.add_argument('--runs', type=int, default=RUNS)
    return result


def arms():
    for volume in VOLUMES:
        for drc in (0, 1):
            yield dict(block='drc', volume=volume, drc=drc)
    for volume in EQ_VOLUMES:
        for cutoff in (0, *CUTOFFS):
            yield dict(block='eq', volume=volume, drc=1, cutoff=cutoff)
    for owner in ('dac', 'adc'):
        for ramp in RAMPS:
            yield dict(block=owner + '-ramp', volume=EQ_VOLUMES[0], drc=1, ramp=ramp)


def music_take(record, args, name, writes=(), tone=False):
    path = args.out.with_name(name + '.wav')
    try:
        if not tone:
            record('sound', 'play', MUSIC, 'loop', GAIN)
        schedule = tuple(word for write in writes for word in ('--at', *write))
        if tone:
            schedule = ('--at', -SETTLE_MS, 'tone', TONE_HZ, TAKE_MS + SETTLE_MS, GAIN, *schedule)
        record('mic', 'record', TAKE_MS, '--settle', SETTLE_MS, *schedule, '-o', str(path), '--json')
    finally:
        record('sound', 'stop', 'all')


def configure(record, saved, arm):
    record('tune', 'audio.codec_volume', arm['volume'])
    record('tune', 'audio.drc', arm['drc'])
    cutoff = arm.get('cutoff', 0)
    program(record, saved, 'dac', eq_design_first_order_highpass(cutoff) if cutoff else None)
    restart(record, saved)


def ramp_take(record, saved, args, arm, name):
    adc = arm['block'] == 'adc-ramp'
    volume_reg = ADC_VOLUME_REG if adc else DAC_VOLUME_REG
    if adc:
        codec(record, ADC_RAMP_REG, (saved[ADC_RAMP_REG] & ~RAMP_MASK) | (arm['ramp'] << RAMP_SHIFT))
        initial = DEFAULTS['alc_gain']
    else:
        record('tune', 'audio.drc_ramp', arm['ramp'])
        initial = codec(record, volume_reg)
    attenuated = initial - REGISTER_STEP
    if not 0 <= attenuated <= BYTE_MAX:
        raise ValueError('volume step outside codec range')
    writes = ((STEP_AT_MS[0], volume_reg, attenuated), (STEP_AT_MS[1], volume_reg, initial))
    try:
        music_take(record, args, name, writes, tone=True)
    finally:
        codec(record, volume_reg, initial)
        codec(record, ADC_RAMP_REG, saved[ADC_RAMP_REG])
        reset_tunables(record, 'audio', ('drc_ramp',))


def restore_playback(record, saved):
    failures = []
    for reg in (ADC_RAMP_REG, ADC_VOLUME_REG, DAC_VOLUME_REG):
        if reg in saved:
            try:
                codec(record, reg, saved[reg])
            except Exception as error:
                failures.append(error)
    for cleanup in (lambda: reset_tunables(record, 'audio', AUDIO_TUNABLES), lambda: restore(record, saved)):
        try:
            cleanup()
        except Exception as error:
            failures.append(error)
    if failures:
        raise RuntimeError('playback restore failed') from failures[0]


def capture(args):
    if args.runs <= 0:
        raise ValueError('runs must be positive')
    with capture_records(args, 'playback DSP capture',
                         lambda command, *words: autana_run(command, '--board', args.board, *words)) as record:
        saved = {}
        try:
            record('status')
            for reg in sorted(set((*REGISTERS, ADC_RAMP_REG, ADC_VOLUME_REG, DAC_VOLUME_REG))):
                saved[reg] = codec(record, reg)
            for name, value in dict(DEFAULTS, eq=0).items():
                record('tune', 'mic.' + name, value)
            record.note('runs', args.runs)
            for index, arm in enumerate(arms()):
                record.note('arm', dict(arm, index=index))
                configure(record, saved, arm)
                for run in range(args.runs):
                    name = f'playback-{index:02d}-{run:02d}'
                    if arm['block'].endswith('-ramp'):
                        ramp_take(record, saved, args, arm, name)
                    else:
                        music_take(record, args, name)
                        frequencies = (TONE_HZ, LOW_TONE_HZ) if arm['block'] == 'eq' else (TONE_HZ,) if arm['volume'] == GAIN else ()
                        for hz in frequencies:
                            record('tone', hz, TONE_MS, GAIN, '--json')
            record('buildid', '--json')
        finally:
            try:
                record('sound', 'stop', 'all')
            finally:
                restore_playback(record, saved)


def main():
    args = parser().parse_args()
    capture(args)
    print(f'Playback DSP capture: {args.out}')


if __name__ == '__main__':
    main()
