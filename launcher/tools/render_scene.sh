#!/bin/sh
#
# The one build-and-render path behind every host render. Source this and
# call `render_scene_run "$@"`; everything that differs between scenes is a
# variable the caller declares first, so a scene script holds declarations
# and no procedure of its own. The same shape tools/device_report.sh uses,
# and for the same reason: a second copy of this procedure is the bug.
#
# An ENGINE scene declares itself in launcher/tools/scenes/; an APP's scene
# declares itself in that app's own tools/scenes/, so nothing here ever
# names an app. Both are found by tools/render_all_scenes.sh.
#
# Declare before sourcing:
#
#   scene_name      output stem, and what the binary reports as its own
#   scene_sources   the firmware translation units to build, space or
#                   newline separated, relative to launcher/
#   scene_renders   one render per line: <label>|<arguments>|<width>x<height>
#                   The declared size is checked against what the binary
#                   says it wrote, so a renderer that has quietly stopped
#                   working fails the run rather than leaving a picture
#                   nobody looks at twice.
#   scene_includes  OPTIONAL extra -I directories, relative to launcher/
#   scene_defines   OPTIONAL extra compiler flags
#   scene_out_dir   OPTIONAL; the default is results/render/<name> under the
#                   nearest tools/ folder above the scene script
#
# POSIX sh, like the rest of this directory.

# Git Bash hands the compiler and this script MSYS paths (/c/...), which the
# Windows python below cannot open. cygpath exists only there, which is also
# the only place the conversion is needed.
render_scene_to_native() {
    if command -v cygpath > /dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

render_scene_run() {
    for _rs_required in scene_name scene_sources scene_renders; do
        eval "_rs_value=\${$_rs_required+set}"
        if [ -z "${_rs_value:-}" ]; then
            echo "ERROR: $_rs_required was never declared - see tools/render_scene.sh" >&2
            return 1
        fi
    done
    : "${scene_includes:=}"
    : "${scene_defines:=}"

    # launcher/, wherever this scene lives: beside tools/render_scene.sh, or
    # further down in an app's own tools/. Found by walking up to the folder
    # that holds this file rather than by counting levels, so moving a scene
    # between the two is not a second thing to edit.
    _rs_here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
    _rs_launcher="$_rs_here"
    while [ ! -f "$_rs_launcher/tools/render_scene.sh" ] && [ "$_rs_launcher" != "/" ]; do
        _rs_launcher=$(dirname "$_rs_launcher")
    done
    if [ ! -f "$_rs_launcher/tools/render_scene.sh" ]; then
        echo "ERROR: no launcher/tools/ above $_rs_here" >&2
        return 1
    fi
    _rs_tools="$_rs_launcher/tools"

    # Results belong to whichever tools/ folder owns the scene - the shared
    # one, or an app's - and both are already gitignored there.
    _rs_owner="$_rs_here"
    while [ "$(basename "$_rs_owner")" != "tools" ] && [ "$_rs_owner" != "/" ]; do
        _rs_owner=$(dirname "$_rs_owner")
    done
    : "${scene_out_dir:=$_rs_owner/results/render/$scene_name}"

    while [ $# -gt 0 ]; do
        case "$1" in
            -o) scene_out_dir="$2"; shift 2 ;;
            *) echo "usage: $0 [-o <dir>]" >&2; return 2 ;;
        esac
    done

    # shellcheck source=./find_cc.sh
    . "$_rs_tools/find_cc.sh"
    if ! _rs_cc=$(find_cc); then
        echo "No C compiler found." >&2
        echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
        echo "  Debian:  sudo apt install build-essential" >&2
        echo "  macOS:   xcode-select --install" >&2
        return 1
    fi

    mkdir -p "$scene_out_dir"
    _rs_bin="$scene_out_dir/${scene_name}_render"
    if [ "${OS:-}" = "Windows_NT" ]; then
        _rs_bin="$_rs_bin.exe"
    fi

    _rs_flags="-I $_rs_launcher/main -I $_rs_launcher/components/microui/include -I $_rs_tools"
    for _rs_inc in $scene_includes; do
        _rs_flags="$_rs_flags -I $_rs_launcher/$_rs_inc"
    done

    _rs_files="$_rs_tools/render_host.c"
    for _rs_src in $scene_sources; do
        _rs_files="$_rs_files $_rs_launcher/$_rs_src"
    done

    # shellcheck disable=SC2086
    "$_rs_cc" -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
        -Wno-unused-variable -O1 $_rs_flags $scene_defines $_rs_files -o "$_rs_bin" || return 1

    _rs_log="$scene_out_dir/render.log"
    echo "$scene_renders" | {
        while IFS= read -r _rs_line; do
            [ -n "$_rs_line" ] || continue
            _rs_label=${_rs_line%%|*}
            _rs_tail=${_rs_line#*|}
            _rs_args=${_rs_tail%%|*}
            _rs_want=${_rs_tail##*|}
            _rs_path="$scene_out_dir/$_rs_label.bmp"

            # shellcheck disable=SC2086
            if ! "$_rs_bin" $_rs_args -o "$_rs_path" 2> "$_rs_log"; then
                cat "$_rs_log" >&2
                echo "FAIL $scene_name/$_rs_label: the renderer exited non-zero" >&2
                exit 1
            fi
            _rs_said=$(sed -n 's/^RENDER [^ ]* \([0-9]*x[0-9]*\).*/\1/p' "$_rs_log")
            if [ "$_rs_said" != "$_rs_want" ]; then
                echo "FAIL $scene_name/$_rs_label: wrote ${_rs_said:-nothing}, declared $_rs_want" >&2
                exit 1
            fi
            echo "ok $scene_name/$_rs_label $_rs_want -> $_rs_path"
        done
    } || return 1
    rm -f "$_rs_log"

    # A .png beside each BMP when Python and Pillow happen to be installed.
    # Neither is a dependency, and nothing here installs one.
    if _rs_python=$(command -v python3 || command -v python); then
        "$_rs_python" "$(render_scene_to_native "$_rs_tools/render_png.py")" \
            "$(render_scene_to_native "$scene_out_dir")" || return 1
    fi
}
