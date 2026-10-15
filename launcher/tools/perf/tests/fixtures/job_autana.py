"""Autana replay with real acquisition fixtures and a token-bearing hold."""
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_perf_compare import FakeAutana as Compare
from test_perf_sweep import FakeAutana as Sweep

root = Path(os.environ["PERF_JOB_TEST_ROOT"])
words = sys.argv[1:]
token_key = "_AUTANA_DEVICE_LOCK_TOKEN"
with (root / "calls.jsonl").open("a") as stream:
    stream.write(json.dumps(dict(words=words, token=os.environ.get(token_key))) + "\n")
if "lock" in words:
    verb = words[words.index("lock") + 1]
    if verb == "take":
        sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "scripts/device"))
        from device_lock import process_alive
        assert process_alive(int(words[words.index("--pid") + 1])), "hold owner is not alive"
        print("job-token")
    elif verb == "run":
        os.environ[token_key] = os.environ.get(token_key) or "job-token"
        try:
            sys.exit(subprocess.call(words[words.index("--") + 1:]))
        finally:
            with (root / "calls.jsonl").open("a") as stream:
                stream.write(json.dumps(dict(words=["lock", "release"], token=os.environ[token_key])) + "\n")
    sys.exit(0)
if "flash" in words:
    sys.exit(0)
if "suite" in words:
    (root / "ready").touch()
    if os.environ.get("PERF_JOB_MODE") == "fail":
        sys.exit(7)
    if os.environ.get("PERF_JOB_MODE") == "interrupt":
        time.sleep(0.5)
        sys.exit(130)
if os.environ["PERF_JOB_KIND"] == "compare":
    if "buildid" in words:
        print("BUILD_ID=" + (root / "build_id").read_text())
        sys.exit(0)
    fake = Compare(root, shifted=True)
    code, lines, _ = fake(words, io.StringIO(), 1800)
    for _, line in lines:
        if "booted BUILD_ID=" in line:
            (root / "build_id").write_text(line.split("BUILD_ID=")[1])
else:
    code, lines, _ = Sweep(root)(words, io.StringIO(), 1800)
for _, line in lines:
    print(line, flush=True)
sys.exit(code)
