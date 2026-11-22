#!/usr/bin/env python3
"""Guard task stack frames measured from the diagnostics build's call graphs.

Derive test roots from registered suites in the diagnostics compile database.
Discover launcher/**/stack_chain.txt: root FUNCTION frame|system|boot and
indirect CALLER... : CALLEE... (private names: file.c:function).
Use PARENT>CALLER when a direct caller binds the dispatcher's target.
The diagnostics build writes each object's graph (.ci) and frames (.su) beside
it; source pointer calls are closed here, context and budgets are derived.
Uncompiled libraries are not counted: the C library's printf family,
esp_log and FreeRTOS. timing.c's board watermark check covers the rest.

    stack_chain_gate.py [build-dir]
"""

import json
import os
from pathlib import Path
import re
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
LAUNCHER = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(LAUNCHER, "tools"))
from build.c_header import read_define
from build.compile_db import compile_flags

MAIN_DIR = os.path.join(LAUNCHER, "main")
CHECKER = os.path.join(LAUNCHER, "test", "check_stack_usage.py")
TIMING = os.path.join(LAUNCHER, "test", "timing.c")


def stack_reserve():
    """The bytes a test must leave free: timing.c's own constant."""
    try:
        return read_define(TIMING, "STACK_RESERVE_BYTES")
    except RuntimeError as error:
        raise SpecError(str(error)) from error

NODE_RE = re.compile(r'node: \{ title: "([^"]*)" label: "([^"]*)"')
EDGE_RE = re.compile(r'edge: \{ sourcename: "([^"]*)" targetname: "([^"]*)"( label:)?')
FRAME_RE = re.compile(r"(\d+) bytes \(static\)")
INDIRECT = "__indirect_call"
KINDS = {"test": "call_protected", "frame": "shell_step_app",
         "system": "scene_shell_render", "boot": "app_boot_init"}


class SpecError(Exception):
    pass


def read_spec(path):
    roots, indirect = [], []
    with open(path, "r", encoding="utf-8") as fh:
        for no, raw in enumerate(fh, 1):
            words = raw.split("#", 1)[0].split()
            if not words:
                continue
            if words[0] == "root" and len(words) == 3 and words[2] in KINDS and words[2] != "test":
                roots.append((words[1], words[2]))
            elif words[0] == "indirect" and words.count(":") == 1:
                split = words.index(":")
                callers, callees = words[1:split], words[split + 1:]
                if not callers or not callees:
                    raise SpecError("%s:%d: 'indirect' needs both sides"
                                    % (path, no))
                indirect.extend((a, b) for a in callers for b in callees)
            else:
                raise SpecError("%s:%d: expected root F KIND or indirect A... : B..."
                                % (path, no))
    return roots, indirect


def source_name(title):
    """The function a graph title names, without its file or clone suffix."""
    return title.rsplit(">", 1)[-1].rsplit(":", 1)[-1].split("$", 1)[0].split(".", 1)[0]


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
            elif re.search(r"\d+ bytes", label):
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
    qualifier, _, function = name.rpartition(":")
    return sorted(t for t in frame if source_name(t) == (function or name)
                  and (not qualifier or qualifier in t.replace("\\", "/")))


def deepest(root, frame, calls, stop=None, allowed=None):
    memo = {}

    def walk(node, path):
        if node in memo:
            return memo[node]
        best = (0, []) if stop is None or node == stop else (-1, [])
        for callee in sorted(calls.get(node, ())):
            if node != stop and callee not in path and (allowed is None or callee in allowed):
                cand = walk(callee, path | {node})
                if cand[0] > best[0]:
                    best = cand
        memo[node] = ((frame.get(node, 0) + best[0], [node] + best[1])
                      if best[0] >= 0 else (-1, []))
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


def harness_path(entries, endpoints, suites, frame, calls, reverse_calls):
    endpoint_ancestors = {end: reachable([end], reverse_calls) for end in endpoints}
    paths = []
    if not suites:
        paths = [deepest(entry, frame, calls, end, allowed) for entry in entries
                 for end, allowed in endpoint_ancestors.items()]
    for title in suites:
        suite_ancestors = reachable([title], reverse_calls)
        before, ancestors = max((deepest(entry, frame, calls, title, suite_ancestors) for entry in entries),
                                default=(-1, []))
        after, descendants = max((deepest(title, frame, calls, end, allowed)
                                  for end, allowed in endpoint_ancestors.items()),
                                 default=(-1, []))
        if before >= 0 and after >= 0:
            paths.append((before + after - frame[title], ancestors + descendants[1:]))
    return max(paths, default=(-1, []))


def dispatch_paths(names, titles_by_name, calls):
    """Compiled direct paths matching the names in a scoped declaration."""
    paths = [()]
    for name in names:
        titles = resolve(name, titles_by_name[name.rsplit(":", 1)[-1]])
        paths = [(*path, title) for path in paths for title in titles
                 if not path or title in calls.get(path[-1], ())]
    return paths


def bind_dispatchers(declared, titles_by_name, frame, calls, pointer_callers, bad):
    """Copy compiled frames along each path that fixes a dispatcher's target."""
    scoped, ordinary, problems = defaultdict(set), [], []
    for caller, target in declared:
        names = caller.split(">")
        if len(names) == 1:
            ordinary.append((caller, target))
            continue
        targets = resolve(target, titles_by_name[target.rsplit(":", 1)[-1]])
        bindings = [path for path in dispatch_paths(names, titles_by_name, calls)
                    if path[-1] in pointer_callers]
        if not targets or not bindings:
            problems.append(f"declared edge {caller} > {target} has no measured dispatch")
        for binding in bindings:
            for length in range(2, len(binding)):
                scoped[binding[:length]]
            scoped[binding].update(targets)
    covered = set()
    for path in sorted(scoped, key=len):
        dispatcher = path[-1]
        parent = ">".join(path[:-1])
        bound = ">".join(path)
        frame[bound] = frame[dispatcher]
        if dispatcher in bad:
            bad.add(bound)
        if dispatcher in pointer_callers:
            pointer_callers.add(bound)
        if scoped[path]:
            covered.add(bound)
        calls[bound] = calls.get(dispatcher, set()) | scoped[path]
        calls[parent].remove(dispatcher)
        calls[parent].add(bound)
    return ordinary, covered, problems


def check_app(name, spec_path, ci_paths, stack_bytes, reserve, context=0, runner_edges=(),
              graph=None, declared=None, test_roots=None):
    roots, own_edges = read_spec(spec_path)
    test_roots = test_roots or {}
    roots.extend((test, "test") for test in test_roots)
    declared = own_edges if declared is None else declared
    frame, calls, pointer_callers, bad = graph or parse_graph(ci_paths)
    frame = frame.copy()
    bad = set(bad)
    pointer_callers = set(pointer_callers)
    calls = {caller: set(callees) for caller, callees in calls.items()}

    titles_by_name = defaultdict(list)
    for title in frame:
        titles_by_name[source_name(title)].append(title)

    declared, covered, problems = bind_dispatchers(
        declared, titles_by_name, frame, calls, pointer_callers, bad)
    for caller, callee in declared + list(runner_edges):
        callers = resolve(caller, titles_by_name[caller.rsplit(":", 1)[-1]])
        callees = resolve(callee, titles_by_name[callee.rsplit(":", 1)[-1]])
        if not callers or (callee != "-" and not callees):
            problems.append("declared edge %s > %s names a function that is "
                            "not in the graph" % (caller, callee))
        for c in callers:
            covered.add(c)
            calls.setdefault(c, set()).update(callees)

    for wrapper in resolve("vPortTaskWrapper", frame):
        calls.setdefault(wrapper, set()).update(resolve("main_task", frame))

    root_titles = {}
    for root, _kind in roots:
        function = root.rsplit(":", 1)[-1]
        candidates = titles_by_name[function]
        found = resolve(root, candidates)
        if not found and root in test_roots:
            found = [title for title in candidates if ":" not in title]
        if not found:
            problems.append("root %s is not in the graph" % root)
        root_titles[root] = found
    for title in sorted(reachable((t for titles in root_titles.values() for t in titles), calls)):
        if title in bad:
            problems.append("%s has a frame that is not a fixed size" % title)
        if title in pointer_callers and title not in covered:
            problems.append("%s makes a call through a pointer that no "
                            "'indirect' line declares" % title)

    entries = resolve("vPortTaskWrapper", frame) or resolve("main_task", frame)
    reverse_calls = defaultdict(set)
    for caller, callees in calls.items():
        for callee in callees:
            reverse_calls[callee].add(caller)
    harnesses_by_kind = {}
    for root, kind in roots:
        titles = root_titles[root]
        suites = test_roots.get(root, ())
        key = (kind, suites)
        if key not in harnesses_by_kind:
            endpoints = titles_by_name[KINDS[kind]]
            suite_titles = [title for suite in suites for title in titles_by_name[suite]]
            harnesses_by_kind[key] = harness_path(entries, endpoints, suite_titles, frame, calls, reverse_calls)
        overhead, ancestors = harnesses_by_kind[key]
        if overhead < 0 or any(frame.get(t, 0) == 0 or t in bad for t in ancestors):
            problems.append("%s: no measured main_task path to %s" % (root, KINDS[kind]))
            continue
        if not titles:
            continue
        total, chain = max(deepest(t, frame, calls) for t in titles)
        used = total + overhead + context
        budget = stack_bytes - reserve - overhead - context
        print("stack_chain_gate: %s %s() chain %d, harness %d, context %d, used %d, free %d (%s)" %
              (name, root, total, overhead, context, used, stack_bytes - used,
               " > ".join("%s %d" % (source_name(n), frame.get(n, 0)) for n in chain)))
        print("stack_chain_gate: harness " + " > ".join(
            "%s %d" % (source_name(n), frame[n]) for n in ancestors))
        if total > budget:
            problems.append("%s() compiled frames use %d bytes; main task %d must leave reserve %d" %
                            (root, used, stack_bytes, reserve))

    return problems


def app_jobs(db_path, app_dir=None, functions=(), source_files=()):
    with open(db_path, "r", encoding="utf-8") as fh:
        entries = json.load(fh)
    marker = (app_dir or MAIN_DIR).replace("\\", "/").rstrip("/") + "/"
    jobs = []
    definition = re.compile(r"\b(?:%s)\s*\([^;{}]*\)\s*\{" %
                            "|".join(re.escape(f) for f in functions)) if functions else None
    for entry in entries:
        source = entry["file"].replace("\\", "/")
        if source_files:
            if not any(source.endswith("/" + name) for name in source_files):
                continue
        elif not source.endswith(".c") or (app_dir is None and "/apps/" in source):
            continue
        elif definition is not None:
            with open(source, "r", encoding="utf-8", errors="replace") as fh:
                if not definition.search(fh.read()):
                    continue
        elif marker not in source:
            continue
        jobs.append((entry, entry["directory"], source, graph_file(entry)))
    return jobs


def graph_file(entry):
    """The call graph GCC writes beside an object: x.c.obj gives x.c.ci."""
    return os.path.splitext(os.path.join(entry["directory"], entry["output"]))[0] + ".ci"


def runner_registrations(jobs):
    """Expand the device's registrations using each suite's own compiler flags."""
    roots = {}
    edges = []
    for entry, cwd, source, _graph in jobs:
        with open(source, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        edges.extend(("UnityDefaultTestRun", target) for target in
                     re.findall(r"UnityDefaultTestRun\(\s*(\w+)\s*,", text)
                     if target != "func")
        suites = re.findall(r"(?m)^\s*SUITE_REGISTER(_ON_REQUEST)?\s*\(\s*(\w+)\s*\)", text)
        if not suites:
            continue
        expanded = subprocess.run([*compile_flags(entry), "-E", "-P", source], cwd=cwd,
                                  capture_output=True, text=True)
        if expanded.returncode:
            raise SpecError(f"{source}: cannot expand suite registrations: {expanded.stderr}")
        suites = [(on_request, suite) for on_request, suite in suites
                  if re.search(rf"\bsuite_link\s*\(\s*&{re.escape(suite)}_entry\s*\)", expanded.stdout)]
        if not suites:
            continue
        for on_request, suite in suites:
            edges.append(("suites_run_request", suite))
            if not on_request:
                edges.append(("suites_run_all", suite))
        tests = set(re.findall(r"\bsuite_run_test_timed\s*\(\s*(\w+)\s*,", expanded.stdout))
        path = Path(source)
        qualifier = path.relative_to(LAUNCHER).as_posix() if path.is_relative_to(LAUNCHER) else path.name
        for test in sorted(tests):
            roots[f"{qualifier}:{test}"] = tuple(suite for _, suite in suites)
    return roots, edges


def main(argv):
    build = os.path.realpath(argv[1] if len(argv) > 1
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

    specs = sorted(str(p) for p in Path(LAUNCHER).rglob("stack_chain.txt")
                   if not p.relative_to(LAUNCHER).parts[0].startswith("build"))
    if not specs:
        raise SpecError("no launcher/**/stack_chain.txt declares a chain")

    shared_jobs = app_jobs(db)
    shared_jobs += app_jobs(db, os.path.join(LAUNCHER, "test"))
    shared_jobs += app_jobs(db, os.path.join(LAUNCHER, "packages"))
    selected = {job[2] for job in shared_jobs}
    shared_jobs += [job for job in app_jobs(db, functions={"main_task", "UnityDefaultTestRun", "vPortTaskWrapper"})
                    if job[2] not in selected]
    entry, cwd = shared_jobs[0][:2]
    cmd = compile_flags(entry)
    probe = subprocess.run(cmd + ["-S", "-x", "c", "-", "-o", "-"], cwd=cwd,
                           input='#include "xtensa_context.h"\nchar stack_chain_context[XT_STK_FRMSZ + ((XT_CP_SIZE + 15) & ~15)];\n',
                           capture_output=True, text=True)
    size = re.search(r"\.size\s+stack_chain_context,\s*(\d+)", probe.stdout)
    if probe.returncode or not size:
        raise SpecError("cannot derive XT_STK_FRMSZ: " + probe.stderr[-600:])
    nm = cmd[0].replace("gcc", "nm")
    symbols = subprocess.run([nm, os.path.join(build, "launcher.elf")],
                             capture_output=True, text=True, check=True).stdout
    tls = {name: int(address, 16) for address, name in re.findall(
        r"(?m)^([0-9a-fA-F]+) \w (_thread_local_(?:data_start|bss_end))$", symbols)}
    if len(tls) != 2:
        raise SpecError("ELF does not define the TLS stack area")
    context = int(size.group(1)) + ((tls["_thread_local_bss_end"] -
                                   tls["_thread_local_data_start"] + 15) & ~15)
    app_sources = [job for job in app_jobs(db, MAIN_DIR) if "/apps/" in job[2]]
    declared = [edge for spec in specs for edge in read_spec(spec)[1]]
    target_files = {target.rsplit(":", 1)[0] for _, target in declared if ":" in target}
    all_jobs = shared_jobs + app_sources
    present = {job[2] for job in all_jobs}
    all_jobs.extend(job for job in app_jobs(db, source_files=target_files) if job[2] not in present)
    ci_paths = [job[3] for job in all_jobs]
    missing = [job[2] for job in all_jobs if not os.path.isfile(job[3])]
    if missing:
        print("stack_chain_gate: no call graph for %d sources (first %s) - rebuild the "
              "diagnostics image, which writes one beside each object" % (len(missing), missing[0]),
              file=sys.stderr)
        return 1
    graph = parse_graph(ci_paths)
    test_roots, runner_edges = runner_registrations(all_jobs)
    if not test_roots:
        raise SpecError("no registered tests in the diagnostics build")
    status = 0
    for spec in specs:
        name = os.path.basename(os.path.dirname(spec))
        try:
            problems = check_app(name, spec, ci_paths, stack_bytes,
                                 stack_reserve(), context, runner_edges, graph, declared,
                                 test_roots if Path(spec).parent == Path(TIMING).parent else ())
        except SpecError as exc:
            problems = [str(exc)]
        for problem in problems:
            print("stack_chain_gate: %s: %s" % (name, problem))
        if problems:
            status = 1
    for spec in specs:
        app_dir = os.path.dirname(spec).replace("\\", "/")
        jobs = [job for job in app_sources if job[2].startswith(app_dir + "/")]
        if not jobs:
            continue
        objects = os.path.commonpath([os.path.dirname(job[3]) for job in jobs])
        if subprocess.run([sys.executable, CHECKER, objects]).returncode != 0:
            status = 1
    print("stack_chain_gate: %s; counts compiled source frames only. "
          "Uncompiled libc printf, esp_log and FreeRTOS "
          "are not counted; timing.c board watermark check covers the rest."
          % ("FAIL" if status else "PASS"))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
