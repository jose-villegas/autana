"""What the doc-figure producers share: building and running a C probe against the engine's libraries."""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.packages import include_roots  # noqa: E402
from docs.probe_standard import PROBE_STANDARD  # noqa: E402

# Every probe prints its numbers through the library's own text formatting.
PROBE_SUPPORT = (ROOT / "launcher/packages/math/src/text/math_text.c",)


def build_probe(cc, sources, directory):
    """Build a probe against the library into directory; returns the program."""
    binary = pathlib.Path(directory) / "probe"
    includes = [flag for root in include_roots() for flag in ("-I", str(root))]
    subprocess.run([cc, f"-std={PROBE_STANDARD}", "-O1", *includes, *map(str, (*sources, *PROBE_SUPPORT)),
                    "-lm", "-o", str(binary)], check=True)
    return binary


def probe_json(binary, *arguments):
    """Run a built probe; its JSON is every number a figure shows."""
    out = subprocess.run([str(binary), *map(str, arguments)], check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def run_probe(cc, sources, *arguments):
    """Build and run a probe once."""
    with tempfile.TemporaryDirectory() as work:
        return probe_json(build_probe(cc, sources, work), *arguments)


def probe_into_folder(description, sources, *arguments):
    """A producer's --cc CC --out DIR: makes DIR and runs the probe; returns (its JSON, DIR)."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    return run_probe(args.cc, sources, *arguments), args.out
