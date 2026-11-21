"""Sweep low tones and record how hard the board shakes (IMU) against how loud it sounds (mic).

Board flat on a desk, untouched, quiet room, on a screen without a motion reader. Run it inside one lock run:
autana --wait 3600 lock run -- python launcher/tools/audio/audio_buzz_sweep.py --board <id> --surface "folded cloth, face up"
"""
import argparse
from datetime import date
import json
from pathlib import Path
from statistics import median
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from audio_drc_sweep import numbers
from autana_runner import autana_run, record_line

CAPTURE = Path("docs/audio/data/audio_buzz_capture.txt")
FIRST_HZ, LAST_HZ, STEP_HZ = 20, 300, 10
# Safety: the speaker is rated 1 W. Long, loud, low tones are what push a small cone past its travel.
MAX_MS = 500
LOW_HZ = 60
LOW_MAX_VOLUME = 60
# The fine pass: this many coarse shake peaks, each walked in FINE_STEP_HZ steps to its coarse neighbours.
PEAKS = 3
FINE_STEP_HZ = 2
# At the buzz rate the sensor FIFO holds 143 ms; a busy core 1 can delay the IMU task's
# default 100 ms drain past that, and the overflow gap empties the window. Drain faster.
DRAIN_TUNABLE = "imu.drain_ms"
DRAIN_MS = 40


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--autana", default="autana")
    result.add_argument("--board", required=True)
    result.add_argument("--surface", required=True, help="supporting surface and board orientation")
    result.add_argument("--out", type=Path, default=ROOT / CAPTURE)
    result.add_argument("--hz", type=numbers, default=tuple(range(FIRST_HZ, LAST_HZ + 1, STEP_HZ)))
    result.add_argument("--volumes", type=numbers, default=numbers("10,30,60"))
    result.add_argument("--loud", type=int, default=100, help="one extra burst per tone at or above LOW_HZ")
    result.add_argument("--ms", type=int, default=400)
    result.add_argument("--runs", type=int, default=3)
    result.add_argument("--rest", type=float, default=1.0, help="seconds of silence between tones")
    result.add_argument("--peaks", type=int, default=PEAKS, help="coarse shake peaks to refine; 0 skips the fine pass")
    return result


def points(args, frequencies):
    for hz in frequencies:
        for run in range(args.runs):
            for volume in args.volumes:
                yield hz, volume
        if args.loud and hz >= LOW_HZ:
            yield hz, args.loud


def safe(hz, volume):
    return hz >= LOW_HZ or volume <= LOW_MAX_VOLUME


def fine_frequencies(shakes, peaks):
    """FINE_STEP_HZ steps between each of the `peaks` strongest local maxima and its coarse neighbours."""
    coarse = sorted(shakes)
    maxima = [hz for index, hz in enumerate(coarse)
              if all(shakes[hz] >= shakes[other] for other in coarse[max(index - 1, 0):index + 2])]
    fine = set()
    for peak in sorted(maxima, key=lambda hz: shakes[hz], reverse=True)[:peaks]:
        position = coarse.index(peak)
        low, high = coarse[max(position - 1, 0)], coarse[min(position + 1, len(coarse) - 1)]
        fine.update(range(low + FINE_STEP_HZ, high, FINE_STEP_HZ))
    return sorted(fine - set(coarse))


def sweep(args, output, frequencies, shakes):
    planned = list(points(args, frequencies))
    if not all(safe(hz, volume) for hz, volume in planned):
        raise ValueError(f"volumes above {LOW_MAX_VOLUME}% need {LOW_HZ} Hz or more")
    levels = {}
    retries = 0
    for hz, volume in planned:
        command = ("tone", str(hz), str(args.ms), str(volume))
        reply = json.loads(autana_run(args.autana, *command, "--json"))
        if reply["shake_rms_mg"] is None:
            # One late drain can cost a window; another screen reading the IMU costs every one.
            retries += 1
            time.sleep(args.rest)
            reply = json.loads(autana_run(args.autana, *command, "--json"))
        if reply["shake_rms_mg"] is None:
            raise RuntimeError(f"{' '.join(command)}: shake n/a twice; is another screen reading the IMU? (board log)")
        record_line(output, " ".join(command), json.dumps(reply))
        if volume == max(args.volumes):
            levels.setdefault(hz, []).append(reply["shake_rms_mg"])
        time.sleep(args.rest)
    shakes.update({hz: median(values) for hz, values in levels.items()})
    output.write(f"# {retries} of {len(planned)} tones retried after a shake n/a\n")


def capture(args):
    if not 0 < args.ms <= MAX_MS:
        raise ValueError(f"tone length must be 1..{MAX_MS} ms")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as output:
        output.write(f"# autana --json replies on {args.board}, {date.today()}, surface={args.surface}, "
                     "one command per line: <command>\t<reply>\n")
        record_line(output, "buildid", autana_run(args.autana, "buildid", "--json").strip())
        tune = ("tune", DRAIN_TUNABLE, str(DRAIN_MS))
        record_line(output, " ".join(tune), json.dumps(autana_run(args.autana, *tune).strip()))
        shakes = {}
        sweep(args, output, args.hz, shakes)
        if args.peaks:
            sweep(args, output, fine_frequencies(dict(shakes), args.peaks), shakes)
        reset = ("tune", "reset", DRAIN_TUNABLE)
        record_line(output, " ".join(reset), json.dumps(autana_run(args.autana, *reset).strip()))
        record_line(output, "buildid", autana_run(args.autana, "buildid", "--json").strip())


def main():
    args = parser().parse_args()
    capture(args)
    print(f"Buzz sweep capture: {args.out}")


if __name__ == "__main__":
    main()
