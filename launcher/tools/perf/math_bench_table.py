#!/usr/bin/env python3
"""Generate the math benchmark from checked-in measurements; --from-elf records wrapper FPU ops."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
sys.path.insert(0, str(ROOT / "launcher/tools/quality"))
sys.path.insert(0, str(ROOT / "launcher/tools/build"))
from generated_blocks import blocks, digest, replace_block
from icount_gate import load_baseline
from code_layout import LayoutError, decode, instructions, skip_pools, symbols
from c_header import read_define

DATA = ROOT / "docs/math/data"
SUITE = ROOT / "launcher/test/suites/suite_math_perf.c"
BASELINE = ROOT / "launcher/tools/quality/icount_baseline.txt"
DOCUMENT = ROOT / "docs/math/Benchmark.md"
BLOCK = "math-benchmark"
PLACEHOLDER = "# Board capture pending: run autana suite math_perf --flash."
FPU_OPS = ("madd.s", "msub.s", "recip0.s", "rsqrt0.s", "div0.s", "divn.s", "sqrt0.s")
NUMBER = r"[0-9]+(?:\.[0-9]+)?"
CAPTURE_ROW = re.compile(
    rf"MATHBENCH name=(\w+) type=(\w+) warm=({NUMBER}) cold=({NUMBER}) insn=({NUMBER})")
SYMBOL = re.compile(r"^[0-9a-fA-F]+ <([^>]+)>:$")
ROW_SYMBOL_PREFIX = "math_bench_"
LABELS = {"mul": "mul (scalar)", "scale": "scale (vec3)", "add": "add (vec3)",
          "dot": "dot (vec3)", "normalize": "normalize (vec3)", "lerp": "lerp (vec3)",
          "quat_mul": "quat mul", "mat4_mul": "mat4 mul",
          "transform_compute_matrix": "transform compute_matrix"}


def suite_rows(path=SUITE):
    text = path.read_text(encoding="utf-8").split("static const row_t rows[] = {", 1)[1].split("};", 1)[0]
    entries = re.findall(r'\{\s*"(\w+)",\s*"(\w+)",\s*(\w+),\s*(true|false),\s*(\w+)\s*\}', text)
    rows = [(name, kind, hot == "true", int(ops) if ops.isdecimal() else read_define(path, ops))
            for name, kind, symbol, hot, ops in entries
            if symbol == ROW_SYMBOL_PREFIX + name + "_" + kind]
    if len(rows) != len(entries):
        raise ValueError("row symbol does not match its name and type")
    if not rows or len({(name, kind) for name, kind, _, _ in rows}) != len(rows):
        raise ValueError("invalid suite row table")
    if any(not isinstance(ops, int) or ops <= 0 for _, _, _, ops in rows):
        raise ValueError("row operation count must be a positive integer")
    return rows


def parse_capture(text, rows):
    if text.strip() == PLACEHOLDER:
        return None, {}
    build = None
    measured = {}
    for line in text.splitlines():
        if line.startswith("BUILD_ID="):
            if build is not None or not re.fullmatch(r"BUILD_ID=[\w.-]+", line):
                raise ValueError("invalid or repeated build id")
            build = line.removeprefix("BUILD_ID=")
        elif "MATHBENCH" in line:
            match = CAPTURE_ROW.fullmatch(line)
            if match is None:
                raise ValueError(f"malformed measurement: {line}")
            name, kind, *values = match.groups()
            key = name, kind
            if key in measured or any(float(value) <= 0 for value in values):
                raise ValueError(f"invalid or duplicate measurement: {line}")
            measured[key] = tuple(map(float, values))
    expected = {(name, kind) for name, kind, _, _ in rows}
    if expected - measured.keys():
        raise ValueError(f"missing capture rows: {sorted(expected - measured.keys())}")
    if measured.keys() - expected:
        raise ValueError(f"unexpected capture rows: {sorted(measured.keys() - expected)}")
    if build is None:
        raise ValueError("missing build id")
    return build, measured


def parse_disassembly(text, rows, resolve=None):
    expected = {f"{name}_{kind}" for name, kind, _, _ in rows}
    found = {}
    current = None
    for line in text.splitlines():
        symbol = SYMBOL.fullmatch(line.strip())
        if symbol:
            current = symbol[1].removeprefix(ROW_SYMBOL_PREFIX) if symbol[1].startswith(ROW_SYMBOL_PREFIX) else None
            if current in expected:
                found[current] = []
        elif current in expected:
            found[current].append(line)
    if expected - found.keys():
        raise ValueError(f"missing wrapper symbols: {sorted(expected - found.keys())}")
    result = {}
    for key in sorted(found):
        body = "\n".join(found[key])
        decoded, again = resolve(ROW_SYMBOL_PREFIX + key, body) if resolve else (decode(text=body), None)
        code = skip_pools(decoded, again)
        mnemonics = {item[2] for item in code}
        registers = {}
        callees = set()
        for _, _, mnemonic, operands in code:
            targets = re.findall(r"<([^>]+)>", operands)
            register = operands.split(",", 1)[0].strip()
            if mnemonic == "call8" and targets:
                callees.add(targets[-1])
                registers.clear()
            elif mnemonic == "callx8":
                callees.add(targets[-1] if targets else registers.get(register, "*" + register))
                registers.clear()
            elif mnemonic == "l32r" and targets and "(" in operands:
                registers[register] = targets[-1]
            else:
                registers.pop(register, None)
        ops = " ".join(op for op in FPU_OPS if op in mnemonics)
        calls = "calls " + ", ".join(sorted(callees)) if callees else ""
        result[key] = "; ".join(part for part in (ops, calls) if part) or "-"
    return result


def read_fpu(path, rows):
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        key, ops = line.split("\t")
        direct, separator, calls = ops.partition("calls ")
        valid_ops = all(op in FPU_OPS for op in direct.removesuffix("; ").split())
        valid_calls = not separator or re.fullmatch(r"[\w.$@+*-]+(?:, [\w.$@+*-]+)*", calls)
        if key in result or (ops != "-" and (not valid_ops or not valid_calls or not ops)):
            raise ValueError(f"invalid FPU row: {line}")
        result[key] = ops
    if set(result) != {f"{name}_{kind}" for name, kind, _, _ in rows}:
        raise ValueError("FPU rows do not match suite")
    return result


def render_table(rows, measured, baseline, fpu):
    lines = ["| function | type | cycles/op warm | cycles/op cold | board insn/op | QEMU insn/op | cycles/insn | FPU ops |",
             "|---|---|---:|---:|---:|---:|---:|---|"]
    for name, kind, hot, ops in rows:
        key = f"{name}_{kind}"
        qemu = "-"
        if hot:
            if "math_" + key not in baseline:
                raise ValueError(f"missing baseline row: math_{key}")
            qemu = f"{baseline['math_' + key] / ops:.1f}"
        values = measured.get((name, kind))
        costs = [f"{value:.1f}" for value in values] if values else ["-"] * 3
        ratio = f"{values[0] / values[2]:.1f}" if values else "-"
        lines.append("| " + " | ".join([LABELS.get(name, name), kind, *costs, qemu, ratio, fpu[key]]) + " |")
    return "\n".join(lines)


def render_document(rows, build, measured, baseline, fpu):
    provenance = f"Board build: `{build}`." if build else "Board capture pending; board columns remain unmeasured."
    body = "\n" + provenance + "\n\n" + render_table(rows, measured, baseline, fpu) + "\n"
    op_names = ", ".join(f"`{op}`" for op in FPU_OPS)
    return f'''# Math benchmark

The board is the timing reference. QEMU counts executed instructions; it does
not model cache stalls, FPU latency or cycles. Only hot-path rows selected in
`suite_math_perf.c` participate in the CI instruction ratchet.
A row whose code differs between the device and QEMU builds stays out of the ratchet.

Warm is the median cycle-counter cost after an untimed kernel call, following
Embench-IoT's cache-warming method with fixed workloads.<sup>[[69]](../Citations.md#69)</sup>
Cold is the median of single calls after cache writeback and invalidation.
Inputs live in internal RAM, matching ESP-DSP's reference data placement.
The cache reset stalls the other core and suspends caches through IDF's flash
guard before invoking the S3 ROM whole-cache operations; cache restoration and
the call boundary precede timing. Board insn/op is one warmed batch counted
by the retired-instruction performance event on core zero, with frame counters
disarmed. Interrupts can contribute to measured samples.

An op is one input element, except `dot_f32_n256`: its op is one full dot
product call, matching ESP-DSP's published ANSI O2 workload for the S3. Its
published reference is 1311 cycles per call; compare this calibration row
against the ANSI cell.<sup>[[68]](../Citations.md#68)</sup> Compiler flags, loads, stores and call
overhead affect the comparison. Kernels retain all result components through
volatile stores, included in both the cycles and instruction columns.

Types use the [math suffixes](README.md#number-types); conversions name source
and destination. `mul (scalar)` is the per-type scalar multiply; `scale (vec3)`
multiplies a vector by a scalar. Transform compute_matrix composes TRS into a
fresh matrix, and quat mul composes rotations. Integer vectors have no
normalize, and fixed-point quaternions have no slerp.

Cycles/insn divides warm cycles by board instructions. FPU ops checks
{op_names} directly in each device ELF wrapper and lists its call targets
as `calls` followed by callee symbols from the same disassembly; callee bodies
are not surveyed. An unresolved indirect target is shown as a starred register.
A `-` means neither the listed mnemonics nor calls occur in the wrapper.
QEMU baseline rows count instructions per wrapper call; the table divides by
the suite row's operation count to report insn/op. QEMU is `-` for table-only
rows and any row whose code differs between device and QEMU builds.

Record a board capture with `autana suite math_perf --flash`, retaining its
`BUILD_ID=` and `MATHBENCH` lines in `data/math_bench_capture.txt`. Record the
matching device ELF's wrapper instructions with
`python launcher/tools/perf/math_bench_table.py --from-elf launcher/build.diag/launcher.elf`.
Regenerate with `python launcher/tools/perf/math_bench_table.py > docs/math/Benchmark.md`.
Inputs are the suite's
row table, the checked-in capture and FPU data, and the instruction baseline.

<!-- generated: {BLOCK} sha256={digest(body)} -->{body}<!-- /generated: {BLOCK} -->

<!-- generated: {BLOCK}-inputs check: python launcher/tools/perf/math_bench_table.py --check -->
<!-- /generated: {BLOCK}-inputs -->
'''


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, default=DATA / "math_bench_capture.txt")
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument("--fpu", type=Path, default=DATA / "math_bench_fpu_ops.txt")
    parser.add_argument("--from-elf", type=Path)
    parser.add_argument("--objdump", help="IDF Xtensa objdump executable")
    parser.add_argument("--check", action="store_true", help="check the measured block against its inputs")
    args = parser.parse_args(argv)
    try:
        rows = suite_rows()
        if args.from_elf:
            sys.path.insert(0, str(ROOT / "launcher/tools/render"))
            from code_layout import find_tool
            objdump = args.objdump or find_tool("objdump")
            command = [objdump, "-d", str(args.from_elf)]
            disassembly = subprocess.run(command, capture_output=True, text=True, check=True).stdout
            bounds = {name: address + size for address, (name, size) in
                      symbols(find_tool("nm"), args.from_elf).items()}

            def resolve(symbol, body):
                return instructions(objdump, args.from_elf, symbol, bounds[symbol], text=body)

            fpu = parse_disassembly(disassembly, rows, resolve=resolve)
            lines = ["# Device ELF wrapper mnemonics and call targets; callee bodies excluded.",
                     "# ELF sha256=" + hashlib.sha256(args.from_elf.read_bytes()).hexdigest()]
            lines.extend(f"{key}\t{fpu[key]}" for key in sorted(fpu))
            args.fpu.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
        else:
            build, measured = parse_capture(args.capture.read_text(encoding="utf-8"), rows)
            document = render_document(rows, build, measured, load_baseline(args.baseline), read_fpu(args.fpu, rows))
            if args.check:
                return int(replace_block(DOCUMENT, BLOCK, blocks(document)[BLOCK][2], check=True))
            print(document, end="")
    except (OSError, ValueError, RuntimeError, LayoutError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"math_bench_table: {error}\n")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(newline="\n")
    sys.exit(main())
