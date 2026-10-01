#!/usr/bin/env python3
"""Fail if an app's deepest call chain outgrew its main-task stack budget.

The shell's frame loop and every test run on the ESP-IDF main task, whose
stack is 3,584 bytes; a test must end with the reserve timing.c names free, and a frame loop that
overflows resets the chip. Neither shows on the host, whose stack is
megabytes and whose frames differ.

An app opts in with launcher/main/apps/<name>/stack_chain.txt:

    # comments
    root     <function> <budget bytes>
    indirect <caller>... : <callee>...

This recompiles that app's sources with the diagnostics image's own compiler
and flags (build.diag/compile_commands.json) plus -fstack-usage and
-fcallgraph-info=su, and sums the deepest chain under each root from the
frames GCC reports. Nothing is linked or flashed.

The call graph cannot see a call through a function pointer, so the app
declares each one as an `indirect` edge, and the gate is closed on both
sides: a function reachable from a root that makes a pointer call at a
source line no declaration names as a caller fails, and so does a declaration
whose caller or callee is not in the graph. The gate cannot know a pointer's
targets, so a new target must be added to the list by hand. GCC also lists
block copies and zeroing it expands late (memcpy, memset) with no source
location; those are not followed. A frame that is not a fixed size fails too.

    launcher/tools/quality/stack_chain_gate.py [build-dir]
"""

import glob
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
LAUNCHER = os.path.normpath(os.path.join(HERE, "..", ".."))
MAIN_DIR = os.path.join(LAUNCHER, "main")
CHECKER = os.path.join(LAUNCHER, "test", "check_stack_usage.py")
TIMING = os.path.join(LAUNCHER, "test", "timing.c")


def stack_reserve():
    """The bytes a test must leave free: timing.c's own constant."""
    with open(TIMING, "r", encoding="utf-8") as fh:
        m = re.search(r"#define STACK_RESERVE_BYTES (\d+)U", fh.read())
    if not m:
        raise SpecError("STACK_RESERVE_BYTES not found in %s" % TIMING)
    return int(m.group(1))

NODE_RE = re.compile(r'node: \{ title: "([^"]*)" label: "([^"]*)"')
EDGE_RE = re.compile(r'edge: \{ sourcename: "([^"]*)" targetname: "([^"]*)"( label:)?')
FRAME_RE = re.compile(r"(\d+) bytes \(static\)")
INDIRECT = "__indirect_call"


class SpecError(Exception):
    pass


def read_spec(path):
    roots, indirect = [], []
    with open(path, "r", encoding="utf-8") as fh:
        for no, raw in enumerate(fh, 1):
            words = raw.split("#", 1)[0].split()
            if not words:
                continue
            if words[0] == "root" and len(words) == 3 and words[2].isdigit():
                roots.append((words[1], int(words[2])))
            elif words[0] == "indirect" and words.count(":") == 1:
                split = words.index(":")
                callers, callees = words[1:split], words[split + 1:]
                if not callers or not callees:
                    raise SpecError("%s:%d: 'indirect' needs both sides"
                                    % (path, no))
                indirect.extend((a, b) for a in callers for b in callees)
            else:
                raise SpecError("%s:%d: not 'root F BYTES' or 'indirect "
                                "A... : B...'" % (path, no))
    if not roots:
        raise SpecError("%s: no root" % path)
    return roots, indirect


def source_name(title):
    """The function a graph title names, without its file or clone suffix."""
    return title.rsplit(":", 1)[-1].split("$", 1)[0].split(".", 1)[0]


def parse_graph(ci_paths):
    """(frame bytes, callees, pointer-call callers, bad frames) by title."""
    frame, calls, pointer_callers, bad = {}, {}, set(), []
    for path in ci_paths:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for m in NODE_RE.finditer(text):
            title, label = m.group(1), m.group(2)
            if title == INDIRECT:
                continue
            sized = FRAME_RE.search(label)
            if sized:
                frame[title] = int(sized.group(1))
            elif re.search(r"\d+ bytes", label) or "dynamic" in label:
                bad.append(title)
            else:
                frame.setdefault(title, 0)
        for m in EDGE_RE.finditer(text):
            src, dst, located = m.group(1), m.group(2), m.group(3)
            if dst == INDIRECT:
                if located:
                    pointer_callers.add(src)
                continue
            calls.setdefault(src, set()).add(dst)
    return frame, calls, pointer_callers, bad


def resolve(name, frame):
    return sorted(t for t in frame if source_name(t) == name)


def deepest(root, frame, calls):
    memo = {}

    def walk(node, path):
        if node in memo:
            return memo[node]
        best = (0, [])
        for callee in sorted(calls.get(node, ())):
            if callee not in path:
                cand = walk(callee, path | {node})
                if cand[0] > best[0]:
                    best = cand
        memo[node] = (frame.get(node, 0) + best[0], [node] + best[1])
        return memo[node]

    return walk(root, frozenset())


def reachable(roots, calls):
    seen, todo = set(), list(roots)
    while todo:
        node = todo.pop()
        if node not in seen:
            seen.add(node)
            todo.extend(calls.get(node, ()))
    return seen


def check_app(name, spec_path, ci_paths, stack_bytes, reserve):
    roots, declared = read_spec(spec_path)
    frame, calls, pointer_callers, bad = parse_graph(ci_paths)
    problems = []
    for title in bad:
        problems.append("%s has a frame that is not a fixed size" % title)

    covered = set()
    for caller, callee in declared:
        callers, callees = resolve(caller, frame), resolve(callee, frame)
        if not callers or not callees:
            problems.append("declared edge %s > %s names a function that is "
                            "not in the graph" % (caller, callee))
        for c in callers:
            covered.add(c)
            calls.setdefault(c, set()).update(callees)

    root_titles = []
    for root, _budget in roots:
        found = resolve(root, frame)
        if not found:
            problems.append("root %s is not in the graph" % root)
        root_titles.extend(found)
    for title in sorted(reachable(root_titles, calls)):
        if title in pointer_callers and title not in covered:
            problems.append("%s makes a call through a pointer that no "
                            "'indirect' line declares" % title)

    for root, budget in roots:
        titles = resolve(root, frame)
        if not titles:
            continue
        total, chain = max(deepest(t, frame, calls) for t in titles)
        print("stack_chain_gate: %s %s() deepest chain %d of %d bytes (%s)" %
              (name, root, total, budget,
               " > ".join("%s %d" % (source_name(n), frame.get(n, 0))
                          for n in chain)))
        if total > budget:
            problems.append("%s() chain is %d bytes over its %d-byte budget; "
                            "the main task has %d and a test must leave %d "
                            "free. Shrink the frames above rather than raise "
                            "the budget" % (root, total - budget, budget,
                                            stack_bytes, reserve))
    return problems


def app_jobs(db_path, app_dir):
    with open(db_path, "r", encoding="utf-8") as fh:
        entries = json.load(fh)
    marker = app_dir.replace("\\", "/").rstrip("/") + "/"
    jobs = []
    for entry in entries:
        source = entry["file"].replace("\\", "/")
        if marker not in source or not source.endswith(".c"):
            continue
        argv, skip = [], 0
        for tok in shlex.split(entry["command"], posix=(os.name != "nt")):
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


def object_name(source, marker_root):
    rel = os.path.relpath(source, marker_root).replace("\\", "/")
    return rel.replace("/", "__")[:-2] + ".o"


def main(argv):
    build = os.path.abspath(argv[1] if len(argv) > 1
                            else os.path.join(LAUNCHER, "build.diag"))
    db = os.path.join(build, "compile_commands.json")
    if not os.path.isfile(db):
        print("stack_chain_gate: no %s - build the diagnostics image first"
              % db, file=sys.stderr)
        return 1
    sys.path.insert(0, os.path.join(LAUNCHER, "tools", "device"))
    import device_profile
    profile = device_profile.load()
    stack_bytes = device_profile.require(profile, "DP_MAIN_TASK_STACK_BYTES", int)

    specs = sorted(glob.glob(os.path.join(MAIN_DIR, "apps", "*", "stack_chain.txt")))
    if not specs:
        print("stack_chain_gate: no apps/*/stack_chain.txt declares a chain; "
              "nothing is budgeted", file=sys.stderr)
        return 1

    status = 0
    for spec in specs:
        app_dir = os.path.dirname(spec)
        name = os.path.basename(app_dir)
        out = os.path.join(build, "stack-chain", name)
        os.makedirs(out, exist_ok=True)
        for old in os.listdir(out):
            os.remove(os.path.join(out, old))
        jobs = app_jobs(db, app_dir)
        if not jobs:
            print("stack_chain_gate: %s has no sources in %s" % (name, db),
                  file=sys.stderr)
            status = 1
            continue

        def compile_one(job):
            cmd, cwd, source = job
            obj = os.path.join(out, object_name(source, MAIN_DIR))
            cmd = cmd + ["-fstack-usage", "-fcallgraph-info=su", "-c", source,
                         "-o", obj]
            return source, subprocess.run(cmd, cwd=cwd, capture_output=True,
                                          text=True)

        failed = False
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
            for source, done in pool.map(compile_one, jobs):
                if done.returncode != 0:
                    failed = True
                    print("stack_chain_gate: %s did not compile:\n%s"
                          % (source, done.stderr[-600:]), file=sys.stderr)
        if failed:
            status = 1
            continue

        ci_paths = sorted(glob.glob(os.path.join(out, "*.ci")))
        try:
            problems = check_app(name, spec, ci_paths, stack_bytes,
                                  stack_reserve())
        except SpecError as exc:
            problems = [str(exc)]
        for problem in problems:
            print("stack_chain_gate: %s: %s" % (name, problem))
        if problems:
            status = 1
        # The per-function ceiling rides along on the same device frames.
        if subprocess.run([sys.executable, CHECKER, out]).returncode != 0:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
