"""Suite registrations shared by project listings and QEMU selection."""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gates"))
from c_lexer import tokens  # noqa: E402

SUITE_REGISTRATION = re.compile(r"^[ \t]*SUITE_REGISTER(_ON_REQUEST)?\(\s*([A-Za-z_]\w*)\s*\)", re.M)


def registered_suites(project):
    project = Path(project)
    found = {}
    for folder in sorted((project / "launcher").glob("*")):
        if folder.name.startswith("build"):
            continue
        for source in folder.rglob("*.c") if folder.is_dir() else [folder]:
            if source.suffix != ".c":
                continue
            text = source.read_text(encoding="utf-8", errors="replace")
            matches = list(SUITE_REGISTRATION.finditer(text))
            spans = list(tokens(text)) if matches else []
            for match in matches:
                if any(start <= match.start(2) < end for _, start, end in spans):
                    continue
                on_request, name = match.groups()
                found[name] = (source.relative_to(project).as_posix(), bool(on_request),
                               "#ifdef DEVICE_BUILD" in text)
    return found
