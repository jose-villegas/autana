#!/bin/sh
#
# Locate the board's serial port, on any platform this project's tooling runs
# on. The COM number Windows assigns changes between machines and between
# re-plugs, so a script that hard-codes one stops being a single click the
# first time the board enumerates somewhere else.
#
# A sourced helper, not a hand-copied twin in each caller - the standing
# idiom here, see tools/find_cc.sh and tools/idf.sh.
#
# Usage - source this file and call find_port():
#
#   . "$TOOLS_DIR/find_port.sh"
#   PORT=$(find_port) || PORT=""      # empty means "let the toolchain look"
#
# $ESPPORT wins if the caller has already set it. Otherwise the board is
# found by its USB identity: Espressif's built-in USB Serial/JTAG enumerates
# as VID 0x303A. The name guesses that follow are for a USB-UART bridge
# board, which has no such identity to match.
#
# pyserial lives in ESP-IDF's environment, so that interpreter is preferred
# over whatever python is on PATH.

find_port_python() {
    _fp_python=$(command -v python3 || command -v python || true)
    for _fp_candidate in "$HOME/.espressif/python_env"/idf*_env/bin/python \
                         "$HOME/.espressif/python_env"/idf*_env/Scripts/python.exe; do
        [ -x "$_fp_candidate" ] && _fp_python="$_fp_candidate"
    done
    printf '%s' "${_fp_python:-}"
}

find_port() {
    if [ -n "${ESPPORT:-}" ]; then
        printf '%s' "$ESPPORT"
        return 0
    fi

    _fp_py=$(find_port_python)
    if [ -n "$_fp_py" ]; then
        _fp_found=$("$_fp_py" -c "
from serial.tools import list_ports
ports = list_ports.comports()
hit = [p.device for p in ports if p.vid == 0x303A] or [p.device for p in ports if 'JTAG' in (p.description or '')]
print(hit[0] if hit else '')
" 2>/dev/null | tr -d '\r' || true)
        if [ -n "$_fp_found" ]; then
            printf '%s' "$_fp_found"
            return 0
        fi
    fi

    for _fp_candidate in /dev/ttyACM0 /dev/ttyUSB0 /dev/cu.usbmodem*; do
        if [ -e "$_fp_candidate" ]; then
            printf '%s' "$_fp_candidate"
            return 0
        fi
    done

    return 1
}
