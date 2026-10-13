#!/bin/sh
# ESP-IDF tools resolution shares the Python owner. Set ESPRESSIF_SCRIPT_DIR
# to this module's folder before sourcing it from POSIX sh.

espressif_tools_root() {
    . "$ESPRESSIF_SCRIPT_DIR/../../../scripts/lib/python.sh"
    _ESPRESSIF_PYTHON=$(find_python) || return 1
    "$_ESPRESSIF_PYTHON" "$ESPRESSIF_SCRIPT_DIR/espressif.py" tools
}
