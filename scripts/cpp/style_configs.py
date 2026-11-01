"""Print a C++ project's .clang-format or .clang-tidy (docs/c-style/Cpp.md).

    python <this file> clang-format > .clang-format
    python <this file> clang-tidy > .clang-tidy

Run from the project's top. Each file carries the generated-file banner with
the command as typed there, so check_generated_files.py fails a copy that
drifts from this checkout.
"""
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve()
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "scripts/gates"))
sys.path.insert(0, str(ROOT / "scripts/lib"))
from py_constant import module_constant  # noqa: E402
from tracked import repository  # noqa: E402

COMPLEXITY_GATE = ROOT / "launcher/tools/quality/complexity_gate.py"
MARKER = "GENERATED FILE - do not edit."

CHECKS = (
    "bugprone-*",
    "performance-*",
    "modernize-use-designated-initializers",
    "modernize-use-ranges",
    "modernize-use-std-numbers",
    "modernize-use-starts-ends-with",
    "modernize-use-integer-sign-comparison",
    "modernize-use-constraints",
    "cppcoreguidelines-owning-memory",
    "cppcoreguidelines-pro-type-member-init",
    "cppcoreguidelines-special-member-functions",
    "cppcoreguidelines-avoid-capturing-lambda-coroutines",
    "misc-const-correctness",
    "readability-use-std-min-max",
    "readability-function-cognitive-complexity",
)


def banner(tool):
    # As invoked, not resolved: the project reaches this file through its own path to the checkout.
    script = pathlib.Path(os.path.relpath(os.path.abspath(__file__), repository())).as_posix()
    return f"# {MARKER}\n# python {script} {tool} > .{tool}\n\n"


def clang_format():
    # The engine's own settings, without its header comment, which describes the engine's repository.
    lines = (ROOT / ".clang-format").read_text(encoding="utf-8").splitlines(keepends=True)
    while lines and (lines[0].startswith("#") or not lines[0].strip()):
        lines.pop(0)
    return banner("clang-format") + "".join(lines)


def clang_tidy():
    threshold = module_constant(COMPLEXITY_GATE, "FAIL_THRESHOLD")
    checks = ",\n".join(f"  {check}" for check in ("-*", *CHECKS))
    return (banner("clang-tidy")
            + f"Checks: >-\n{checks}\n"
            + "WarningsAsErrors: '*'\n"
            # The engine's headers come in as system headers, which are never reported.
            + "HeaderFilterRegex: '.*'\n"
            + "CheckOptions:\n"
            + f"  readability-function-cognitive-complexity.Threshold: {threshold}\n")


if __name__ == "__main__":
    tools = {"clang-format": clang_format, "clang-tidy": clang_tidy}
    if len(sys.argv) != 2 or sys.argv[1] not in tools:
        sys.exit(f"usage: {sys.argv[0]} {'|'.join(tools)}")
    # Bytes, so a Windows console writes LF as git stores it.
    sys.stdout.buffer.write(tools[sys.argv[1]]().encode("utf-8"))
