#!/bin/sh
# This shell adapter owns no selection rules; espressif.py owns them.
# Set ESPRESSIF_SCRIPT_DIR before sourcing: POSIX sh has no portable way
# for a sourced file to find itself ($0 names its caller).

espressif_tools_root() {
    . "$ESPRESSIF_SCRIPT_DIR/../../../scripts/lib/python.sh"
    _ESPRESSIF_PYTHON=$(find_python) || return 1
    "$_ESPRESSIF_PYTHON" "$ESPRESSIF_SCRIPT_DIR/espressif.py" tools
}
