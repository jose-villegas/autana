#!/bin/sh
# espressif_tools_root for shell scripts: the tools paired with the ESP-IDF
# espressif.py selects (it owns the rule).
#
#   ESPRESSIF_SCRIPT_DIR=launcher/tools/build; . "$ESPRESSIF_SCRIPT_DIR/espressif.sh"
#   for gcc in "$(espressif_tools_root)"/tools/xtensa-esp-elf/*/...; do
#
# Set ESPRESSIF_SCRIPT_DIR before sourcing: POSIX sh has no portable way
# for a sourced file to find itself ($0 names its caller).

espressif_tools_root() {
    . "$ESPRESSIF_SCRIPT_DIR/../../../scripts/lib/python.sh"
    _ESPRESSIF_PYTHON=$(find_python) || return 1
    "$_ESPRESSIF_PYTHON" "$ESPRESSIF_SCRIPT_DIR/espressif.py" tools
}
