"""Play a WAV through a host output device, with gain in dB."""
import argparse
import math
import sys

import soundfile as sf
import numpy as np

DB_AMPLITUDE = 20
NO_OUTPUT = 'no output device; connect or enable an audio output device and retry'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wav', nargs='?')
    parser.add_argument('--gain', type=float, default=0)
    parser.add_argument('--device', help='output device name or index (default: system output)')
    parser.add_argument('--list', action='store_true', help='list output devices and exit')
    args = parser.parse_args(argv)
    if not args.list and not args.wav:
        parser.error('a WAV is required unless --list is used')
    if not math.isfinite(args.gain):
        parser.error('gain must be finite')
    try:
        import sounddevice as sd
    except OSError:
        print('host_play: PortAudio runtime missing; install libportaudio2 on Linux '
              'or reinstall sounddevice on Windows/macOS', file=sys.stderr)
        return 1
    except ImportError:
        print('host_play: PortAudio Python binding missing; install '
              'launcher/tools/audio/mic/requirements.txt with pip', file=sys.stderr)
        return 1
    try:
        devices = sd.query_devices()
        apis = sd.query_hostapis()
        outputs = [(index, device) for index, device in enumerate(devices)
                   if device['max_output_channels'] > 0]
        if not outputs:
            print(f'host_play: {NO_OUTPUT}', file=sys.stderr)
            return 1
        if args.list:
            for index, device in outputs:
                print(f"Output device {index}: {device['name']} "
                      f"({apis[device['hostapi']]['name']})")
            return 0
        device_id = args.device
        if device_id is not None:
            try:
                device_id = int(device_id)
            except ValueError:
                pass
        device = sd.query_devices(device_id, 'output')
        x, rate = sf.read(args.wav, dtype='float32')
        if not x.size:
            parser.error('WAV contains no samples')
        try:
            x = x * 10 ** (args.gain / DB_AMPLITUDE)
        except OverflowError:
            parser.error('gain would clip; lower --gain')
        peak = float(np.max(np.abs(x)))
        if not np.isfinite(x).all() or peak > 1:
            parser.error('gain would clip; lower --gain')
        peak_dbfs = DB_AMPLITUDE * math.log10(peak) if peak else -math.inf
        duration = len(x) / rate
        print(f"Playing: device={device['name']} "
              f"host_api={apis[device['hostapi']]['name']} rate={rate} Hz "
              f"duration={duration:.3f} s gain={args.gain:.2f} dB "
              f"peak={peak_dbfs:.2f} dBFS", file=sys.stderr, flush=True)
        sd.play(x, rate, device=device_id)
        status = sd.wait()
        print(f'Played: {duration:.3f} s status={status or "OK"}', file=sys.stderr)
        return 0
    except (sd.PortAudioError, ValueError, OSError, RuntimeError) as exc:
        detail = ' '.join(str(exc).splitlines())
        print(f'host_play: {detail}; check the output device with --list '
              'and select it with --device', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
