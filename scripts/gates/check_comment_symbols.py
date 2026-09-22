#!/usr/bin/env python3
"""Fail on a comment naming a function that does not exist.

    python scripts/gates/check_comment_symbols.py [root]

A trim that garbles a cited name leaves a comment pointing at nothing, which
is worse than the long comment it replaced. Anything written as `name()` in a
comment must appear as a declaration or a call somewhere under `root`.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from code_vocabulary import names  # noqa: E402
from check_comment_length import EXCLUDED, scan  # noqa: E402

SKIP = ("build", "build.dev", "build.diag", "build.qemu", "build.qemu.perf", "build.qemu.shell", "managed_components")
CITED = re.compile(r"\b([a-z_][a-z0-9_]{4,})\(\)")

# Named in comments as the C library or the vendor SDK spells them, with no
# definition in this tree to find.
FOREIGN = {"printf", "malloc", "bsp_display_new", "esp_clk_cpu_freq", "xtensa_perfmon_exec",
           "strcasecmp", "strdup"}


def scan_citations(root):
    """Every `name()` cited in a comment under `root`, FOREIGN included, as
    {name: {"path:line", ...}}."""
    cited = {}
    for p in sorted(pathlib.Path(root).rglob("*")):
        if p.suffix not in (".c", ".h") or any(s in p.parts for s in SKIP):
            continue
        rp = p.as_posix()
        if any(rp.startswith(e) for e in EXCLUDED):
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        for c in scan(rp, text):
            for name in CITED.findall(c.text):
                cited.setdefault(name, set()).add(f"{rp}:{c.line}")
    return cited


def stale_foreign(root):
    """FOREIGN entries no comment under `root` actually cites - dead
    allowlisting, the same trap an unused doc-citation allowlist entry is."""
    cited = scan_citations(root)
    return sorted(name for name in FOREIGN if name not in cited)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "launcher"
    cited = scan_citations(root)
    defined, _ = names(root)
    missing = {n: v for n, v in cited.items() if n not in FOREIGN and n not in defined}
    stale = stale_foreign(root)
    for name, where in sorted(missing.items()):
        for site in sorted(where):
            print(f"{site}: comment names {name}(), which does not exist")
    for name in stale:
        print(f"FOREIGN: stale entry {name}: no comment cites it")
    print(f"{len(cited)} symbols cited in comments, {len(missing)} missing, "
          f"{len(stale)} stale FOREIGN entr{'y' if len(stale) == 1 else 'ies'}")
    return 1 if missing or stale else 0


if __name__ == "__main__":
    sys.exit(main())
