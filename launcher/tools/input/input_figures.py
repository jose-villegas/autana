"""Shared command setup for probe-driven motion reference figures."""
import argparse
from pathlib import Path
import sys

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "math"))
from math_figures import ROOT, run_probe


def probe_data(script, motion_source, description):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    probe = Path(script).with_name(Path(script).stem + "_probe.c")
    sources = (probe, ROOT / "launcher/main/input/motion" / motion_source)
    return run_probe(args.cc, sources), args.out
