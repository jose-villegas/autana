"""A stand-in ESP-IDF for the scripts that run idf.py through an export script."""

import os
import sys
from pathlib import Path


def stub_esp_idf(stubs, idf_py_source):
    """Writes into `stubs` an idf.py that runs `idf_py_source` with Python,
    and the export script that puts it first on PATH; returns that script."""
    stubs = Path(stubs)
    stubs.mkdir(parents=True, exist_ok=True)
    (stubs / "idf_stub.py").write_text(idf_py_source)
    (stubs / "tools/cmake").mkdir(parents=True, exist_ok=True)
    (stubs / "tools/cmake/version.cmake").write_text("set(IDF_VERSION_MAJOR 6)\nset(IDF_VERSION_MINOR 1)\n")
    # One stub per platform: cmd would run a POSIX idf.py found first on
    # PATH through the .py file association instead of idf.py.bat.
    if os.name == "nt":
        (stubs / "idf.py.bat").write_bytes(
            f'@"{sys.executable}" "{stubs / "idf_stub.py"}" %*\r\n'.encode())
        export = stubs / "export.bat"
        export.write_bytes(f'@set "PATH={stubs};%PATH%"\r\n'.encode())
        return export
    (stubs / "idf.py").write_text(f'#!/bin/sh\nexec "{sys.executable}" "{stubs / "idf_stub.py"}" "$@"\n')
    (stubs / "idf.py").chmod(0o755)
    export = stubs / "export.sh"
    export.write_text(f'PATH="{stubs.as_posix()}:$PATH"\n')
    # ESP-IDF's export puts its own interpreter on PATH as plain `python`.
    (stubs / "python").write_text(f'#!/bin/sh\nexec "{sys.executable}" "$@"\n')
    (stubs / "python").chmod(0o755)
    return export
