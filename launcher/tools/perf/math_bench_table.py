#!/usr/bin/env python3
"""Update the math benchmark block; --from-elf records wrapper metadata and FPU ops."""
import argparse
import hashlib
import math
from pathlib import Path
import re
import sys
import subprocess

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "launcher/tools" / folder) for folder in ("render", "quality", "perf")]
from generated_blocks import replace_block
from icount_gate import load_baseline
from code_layout import LayoutError, find_tool, instructions, parse_objdump, run, skip_pools, symbols
from capture import build_id

DATA = ROOT / "docs/math/data"
BASELINE = ROOT / "launcher/tools/quality/icount_baseline.txt"
DOCUMENT = ROOT / "docs/math/Benchmark.md"
BLOCK = "math-benchmark"
PLACEHOLDER = "# Board capture pending: run autana suite math_perf --flash."
FPU_OPS = ("madd.s", "msub.s", "recip0.s", "rsqrt0.s", "div0.s", "divn.s", "sqrt0.s")
DEVICE_ONLY_FPU_OPS = ("recip0.s", "rsqrt0.s")
DEVICE_ONLY_INSTRUCTION = "device-only instruction"
# ESP-DSP docs/esp_bm_results.csv at 3c8ac0fdfec8: S3, ANSI, O2, N=256 (ESP-DSP Library Benchmarks).
ESP_DSP_DOTPROD_F32_N256_ANSI_O2_CYCLES = 1311
NUMBER = r"[0-9]+(?:\.[0-9]+)?"
CAPTURE_ROW = re.compile(
    rf"MATHBENCH name=(.+) type=(\w+) ops=([1-9][0-9]*) warm=({NUMBER}) cold=({NUMBER}) insn=({NUMBER})")
SYMBOL = re.compile(r"^[0-9a-fA-F]+ <([^>]+)>:$")
ROW_SYMBOL_PREFIX = "math_bench_"
MANIFEST = "math_bench_manifest"
CALL_SYMBOL = re.compile(r"[\w.$@]+")


def parse_target(text):
    targets = set()
    for line in text.splitlines():
        if not line.startswith("MATHTARGET"):
            continue
        match = re.fullmatch(r"MATHTARGET target=(\w+) arch=([\w-]+)(?: core=([\w.-]+))? execution=(hardware|qemu|host)", line)
        if not match:
            raise ValueError(f"malformed math target: {line}")
        targets.add(match.groups())
    if len(targets) > 1:
        raise ValueError("mixed math capture targets")
    if not targets:
        return None
    target, architecture, core, execution = targets.pop()
    if execution != "hardware":
        raise ValueError("math target measurements require hardware execution")
    return f"{target} ({architecture}{', ' + core if core else ''})"


def parse_capture(text, rows, allow_missing=False):
    if text.rstrip("\r\n") == PLACEHOLDER:
        return None, {}
    build = build_id(text)
    expected = {(label, kind): (key, ops) for key, label, kind, ops in rows}
    measured = {}
    for line in text.splitlines():
        if not line.startswith("MATHBENCH"):
            continue
        match = CAPTURE_ROW.fullmatch(line)
        if match is None:
            raise ValueError(f"malformed measurement: {line}")
        label, kind, ops, *values = match.groups()
        if (label, kind) not in expected:
            raise ValueError(f"unexpected capture row: {label} {kind}")
        key, count = expected[label, kind]
        if key in measured or int(ops) != count or any(float(value) <= 0 for value in values):
            raise ValueError(f"invalid or duplicate measurement: {line}")
        measured[key] = tuple(map(float, values))
    if not allow_missing and len(measured) != len(rows):
        raise ValueError("missing capture rows")
    return build, measured


def parse_disassembly(text, rows, resolve=None):
    expected = {key for key, *_ in rows}
    found = {}
    current = None
    for line in text.splitlines():
        symbol = SYMBOL.fullmatch(line.strip())
        if symbol and "+" not in symbol[1]:
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
        decoded, again = resolve(ROW_SYMBOL_PREFIX + key, body) if resolve else (parse_objdump(body), None)
        code = skip_pools(decoded, again)
        mnemonics = {item[2] for item in code}
        registers = {}
        callees = set()
        for _, _, mnemonic, operands in code:
            targets = re.findall(r"<([^>]+)>", operands)
            register = operands.split(",", 1)[0].strip()
            if mnemonic == "call8" and targets:
                if not CALL_SYMBOL.fullmatch(targets[-1]):
                    raise ValueError(f"invalid call target in {key}: {operands}")
                callees.add(targets[-1])
                registers.clear()
            elif mnemonic == "callx8":
                callees.add(targets[-1] if targets and CALL_SYMBOL.fullmatch(targets[-1])
                            else registers.get(register, "*" + register))
                registers.clear()
            elif mnemonic == "l32r" and targets and "(" in operands and CALL_SYMBOL.fullmatch(targets[-1]):
                registers[register] = targets[-1]
            else:
                registers.pop(register, None)
        ops = " ".join(op for op in FPU_OPS if op in mnemonics)
        calls = "calls " + ", ".join(sorted(callees)) if callees else ""
        result[key] = "; ".join(part for part in (ops, calls) if part) or "-"
    return result


def parse_row(line):
    fields = line.split("\t")
    if len(fields) != 4:
        raise ValueError(f"expected four tab-separated row fields: {line}")
    key, label, kind, ops = fields
    if not re.fullmatch(r"\w+", key) or not label or not re.fullmatch(r"\w+", kind) or not ops.isdecimal() or int(ops) <= 0:
        raise ValueError(f"invalid row metadata: {line}")
    return key, label, kind, int(ops)


def read_fpu(path):
    rows, result = [], {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        if line.count("\t") != 4:
            raise ValueError(f"expected five tab-separated FPU fields: {line}")
        metadata, ops = line.rsplit("\t", 1)
        row = parse_row(metadata)
        direct, separator, calls = ops.partition("calls ")
        valid_ops = all(op in FPU_OPS for op in direct.removesuffix("; ").split())
        valid_calls = not separator or re.fullmatch(r"[\w.$@+*-]+(?:, [\w.$@+*-]+)*", calls)
        if row[0] in result:
            raise ValueError(f"duplicate FPU key: {row[0]}")
        if ops != "-" and (not valid_ops or not valid_calls or not ops):
            raise ValueError(f"invalid FPU row: {line}")
        rows.append(row)
        result[row[0]] = ops
    if not rows or len({(label, kind) for _, label, kind, _ in rows}) != len(rows):
        raise ValueError("missing or duplicate FPU rows")
    return rows, result


def parse_errors(text):
    result = {}
    pattern = re.compile(
        rf"MATHERROR name=(.+) type=(\w+) max=({NUMBER}(?:[eE][+-]?[0-9]+)?) "
        r"unit=(ulp|relative|lsb) method=(proof|sweep)")
    for line in text.splitlines():
        if not line.startswith("MATHERROR "):
            continue
        match = pattern.fullmatch(line)
        if match is None:
            raise ValueError(f"malformed error measurement: {line}")
        label, kind, maximum, unit, method = match.groups()
        if not math.isfinite(float(maximum)):
            raise ValueError(f"nonfinite error measurement: {line}")
        key = (label, kind)
        if key in result:
            raise ValueError(f"duplicate error measurement: {line}")
        result[key] = f"{maximum} {unit} ({method})"
    return result


def parse_bins(text):
    result = {}
    number = rf"{NUMBER}(?:[eE][+-]?[0-9]+)?"
    pattern = re.compile(rf"MATHERRORBIN name=(.+) type=(\w+) x=({number}) max=({number}) unit=(ulp|relative|lsb)")
    for line in text.splitlines():
        if not line.startswith("MATHERRORBIN"):
            continue
        match = pattern.fullmatch(line)
        if match is None:
            raise ValueError(f"malformed error bin: {line}")
        label, kind, x, maximum, unit = match.groups()
        x, maximum = float(x), float(maximum)
        bins = result.setdefault((label, kind), [])
        if not math.isfinite(x) or not math.isfinite(maximum) or any(row[0] == x for row in bins):
            raise ValueError(f"nonfinite or duplicate error bin: {line}")
        if bins and bins[0][2] != unit:
            raise ValueError(f"inconsistent error bin unit: {line}")
        bins.append((x, maximum, unit))
    return result


def render_table(rows, measured, baseline, fpu, build=None, errors=None):
    keys = {key for key, *_ in rows}
    missing = {name for name in baseline if name.startswith("math_") and name[5:] not in keys}
    if missing:
        raise ValueError(f"baseline rows missing from table: {sorted(missing)}")
    lines = [
             "| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |",
             "|---|---|---:|---:|---:|---:|---:|---|---|"]
    bounds = dict(errors or {})
    unmatched = set(bounds) - {(label, kind) for _, label, kind, _ in rows}
    if unmatched:
        raise ValueError(f"error measurements match no row: {sorted(unmatched)}")
    for key, label, kind, ops in rows:
        qemu = f"{baseline['math_' + key] / ops:.1f}" if "math_" + key in baseline else "-"
        if set(fpu[key].partition(";")[0].split()) & set(DEVICE_ONLY_FPU_OPS):
            qemu = DEVICE_ONLY_INSTRUCTION
        values = measured.get(key)
        costs = [f"{value:.1f}" for value in values] if values else ["-"] * 3
        ratio = f"{values[0] / values[2]:.1f}" if values else "-"
        lines.append("| " + " | ".join([label, kind, *costs, qemu, ratio, fpu[key], bounds.get((label, kind), "-")]) + " |")
    reference = ESP_DSP_DOTPROD_F32_N256_ANSI_O2_CYCLES
    calibration = measured.get("dot_f32_n256_f")
    warm = f"{calibration[0]:.1f}" if calibration else "pending"
    ratio = f"{calibration[0] / reference:.1%}" if calibration else "pending"
    lines.extend(["", f"ESP-DSP calibration: measured warm {warm} cycles/call; ANSI O2 reference {reference} cycles/call; measured/reference {ratio}."])
    return "\n".join(lines)


def elf_rows(objdump, elf, located):
    address, size = next(((record.address, record.size) for record in located if record.name == MANIFEST), (None, None))
    if address is None:
        raise ValueError("missing ELF row manifest")
    dump = run(objdump, "-s", f"--start-address={address}", f"--stop-address={address + size}", str(elf))
    data = bytearray()
    for line in dump.splitlines():
        match = re.match(r"^\s*[0-9a-fA-F]+\s+((?:[0-9a-fA-F]{2})+(?: (?:[0-9a-fA-F]{2})+)*)(?:\s{2}|$)", line)
        if match:
            data.extend(bytes.fromhex(match[1]))
    rows = [parse_row(line) for line in data[:size].rstrip(b"\0").decode().splitlines()]
    if not rows or len({key for key, *_ in rows}) != len(rows):
        raise ValueError("missing or duplicate ELF rows")
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, default=DATA / "math_bench_capture.txt")
    parser.add_argument("--error-capture", type=Path, help="math_numbers capture with scalar proofs and component sweeps")
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument("--fpu", type=Path, default=DATA / "math_bench_fpu_ops.txt")
    parser.add_argument("--from-elf", type=Path)
    parser.add_argument("--objdump", help="IDF Xtensa objdump executable")
    parser.add_argument("--nm", help="IDF Xtensa nm executable")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.from_elf:
            objdump = args.objdump or find_tool("objdump")
            located = symbols(args.nm or find_tool("nm"), args.from_elf, records=True)
            rows = elf_rows(objdump, args.from_elf, located)
            bounds = {record.name: record.address + record.size for record in located if record.size is not None}
            missing = [ROW_SYMBOL_PREFIX + key for key, *_ in rows if ROW_SYMBOL_PREFIX + key not in bounds]
            if missing:
                raise ValueError(f"missing wrapper symbols: {missing}")
            def resolve(symbol, body):
                decoded, again = instructions(objdump, args.from_elf, symbol, bounds[symbol])
                return [ins for ins in decoded if ins[0] < bounds[symbol]], again
            fpu = parse_disassembly(run(objdump, "-d", str(args.from_elf)), rows, resolve)
            lines = ["# Device ELF wrapper metadata, mnemonics and call targets; callee bodies excluded.",
                     "# ELF sha256=" + hashlib.sha256(args.from_elf.read_bytes()).hexdigest()]
            lines.extend("\t".join(map(str, (*row, fpu[row[0]]))) for row in sorted(rows))
            args.fpu.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
        else:
            rows, fpu = read_fpu(args.fpu)
            build, measured = parse_capture(args.capture.read_text(encoding="utf-8"), rows, allow_missing=True)
            changed = replace_block(DOCUMENT, BLOCK, render_table(rows, measured, load_baseline(args.baseline), fpu, build,
                                                 parse_errors((args.error_capture or args.capture).read_text(encoding="utf-8"))),
                                    check=args.check, command="python launcher/tools/perf/math_bench_table.py --check")
            return int(args.check and changed)
    except (OSError, ValueError, RuntimeError, LayoutError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"math_bench_table: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
