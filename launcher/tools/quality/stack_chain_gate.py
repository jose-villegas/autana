#!/usr/bin/env python3
"""Fail if the sand step's deepest call chain outgrew its stack budget.

The shell's frame loop and every test run on the ESP-IDF main task, whose
stack is 3,584 bytes. A test that ends with less than 512 bytes free fails
by name on the board; a frame loop that gets there resets the chip. Neither
shows on the host, whose stack is megabytes and whose frames differ.

So this recompiles the sand app's sources with the DIAGNOSTICS image's own
compiler and flags, from build.diag/compile_commands.json, adding
-fstack-usage and -fcallgraph-info=su, and hands the result to
check_stack_usage.py --target device, which sums the deepest chain under each
root named by DP_STACK_CHAIN_BUDGETS_DEVICE in the device profile. A chain
that grows past its budget fails here, in CI, instead of intermittently on a
board. Nothing is linked or flashed; it costs seconds after a build.

    launcher/tools/quality/stack_chain_gate.py [build-dir]
"""

import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
LAUNCHER = os.path.normpath(os.path.join(HERE, "..", ".."))
CHECKER = os.path.join(LAUNCHER, "test", "check_stack_usage.py")
SAND_DIR = "/launcher/main/apps/sand/"


def sand_commands(db_path):
    """(argv, cwd, source) for every sand translation unit in the database."""
    with open(db_path, "r", encoding="utf-8") as fh:
        entries = json.load(fh)
    jobs = []
    for entry in entries:
        source = entry["file"].replace("\\", "/")
        if SAND_DIR not in source or not source.endswith(".c"):
            continue
        argv = []
        tokens = shlex.split(entry["command"], posix=(os.name != "nt"))
        skip = 0
        for tok in tokens:
            if skip:
                skip -= 1
            elif tok in ("-o", "-MT", "-MF", "-c"):
                skip = 1
            elif tok in ("-MD", "-MMD"):
                continue
            elif tok.startswith('@"'):
                argv.append("@" + tok[2:-1])
            else:
                argv.append(tok)
        jobs.append((argv, entry["directory"], source))
    return jobs


def main(argv):
    build = os.path.abspath(argv[1] if len(argv) > 1
                            else os.path.join(LAUNCHER, "build.diag"))
    db = os.path.join(build, "compile_commands.json")
    if not os.path.isfile(db):
        print("stack_chain_gate: no %s - build the diagnostics image first"
              % db, file=sys.stderr)
        return 1
    out = os.path.join(build, "stack-chain")
    os.makedirs(out, exist_ok=True)
    for name in os.listdir(out):
        os.remove(os.path.join(out, name))

    jobs = sand_commands(db)
    if not jobs:
        print("stack_chain_gate: no sand sources in %s" % db, file=sys.stderr)
        return 1

    def compile_one(job):
        cmd, cwd, source = job
        obj = os.path.join(out, os.path.basename(source)[:-2] + ".o")
        cmd = cmd + ["-fstack-usage", "-fcallgraph-info=su", "-c", source,
                     "-o", obj]
        done = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        return source, done

    failed = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for source, done in pool.map(compile_one, jobs):
            if done.returncode != 0:
                failed += 1
                print("stack_chain_gate: %s did not compile:\n%s"
                      % (source, done.stderr[-600:]), file=sys.stderr)
    if failed:
        return 1
    return subprocess.run([sys.executable, CHECKER, out, "--target", "device"]
                          ).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
