#!/bin/sh
#
# Where ESP-IDF's tool installer put its toolchains and python environments,
# for shell scripts: the peer of espressif.py's espressif_tools_root(), which
# owns the rule. Source it, then call espressif_tools_root.
#
#   . "$(dirname "$0")/espressif.sh"
#   for gcc in "$(espressif_tools_root)"/tools/xtensa-esp-elf/*/...; do

espressif_tools_root() {
    printf '%s\n' "${IDF_TOOLS_PATH:-$HOME/.espressif}"
}
