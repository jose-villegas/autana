"""Capture the audio-path table points on a flashed development image.

Run inside one autana lock run; --board <id> identifies capture provenance.
"""
import argparse
from datetime import date
import json
from pathlib import Path

from audio_doc_tables import CAPTURE, CAPTURE_COMMANDS, ROOT
from autana_runner import autana_run, record_line


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--board", required=True)
    result.add_argument("--autana", default="autana")
    result.add_argument("--out", type=Path, default=ROOT / CAPTURE)
    return result


def capture(args):
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as output:
        output.write(f"# autana --json replies on {args.board}, {date.today()}, "
                     "one command per line: <command>\t<reply>\n")
        for command in CAPTURE_COMMANDS:
            reply = json.loads(autana_run(args.autana, *command, "--json"))
            record_line(output, " ".join(command), json.dumps(reply, allow_nan=False))


def main():
    args = parser().parse_args()
    capture(args)
    print(f"Audio path capture: {args.out}")


if __name__ == "__main__":
    main()
