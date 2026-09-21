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
#                   nobody looks at twice. --video (a flag to this script,
#                   not a declaration) also writes each render's frames to
#                   <label>.avi beside its .bmp; it never touches the pinned
#                   hash, which is taken from the .bmp alone.
#   scene_includes  OPTIONAL extra -I directories, relative to launcher/
#   scene_defines   OPTIONAL extra compiler flags
#   scene_out_dir   OPTIONAL; the default is results/render/<name> under the
#                   nearest tools/ folder above the scene script
#   scene_baseline  OPTIONAL; the default is <name>_render_baseline.txt
#                   beside the scene script. Each render's content hash is
#                   checked against it, so a change to the pixels fails the
#                   run instead of passing unseen. Re-pinning is a
#                   deliberate act: --update-baseline, after looking at the
#                   images. A render with no pinned hash yet says so and
#                   passes.
#   scene_pin       OPTIONAL, 1 by default. A scene whose pixels are not
#                   guaranteed identical on every compiler and C library
#                   declares 0 and says why: it is then checked for its
#                   declared size only, since a pin that can fail for a
#                   reason nobody changed teaches the reader to ignore it.
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

# Empty where the platform has neither, which turns the pinned-hash check
# into a notice rather than a silent pass - see render_scene_run() below.
render_scene_sha256() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum > /dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        printf ''
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
    : "${scene_pin:=1}"

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

    _rs_repin=0
    _rs_video=0
    while [ $# -gt 0 ]; do
        case "$1" in
            -o) scene_out_dir="$2"; shift 2 ;;
            --update-baseline) _rs_repin=1; shift ;;
            --video) _rs_video=1; shift ;;
            *) echo "usage: $0 [-o <dir>] [--update-baseline] [--video]" >&2; return 2 ;;
        esac
    done

    # Beside the scene that owns it, so deleting an app takes its pinned
    # hashes with it.
    : "${scene_baseline:=$_rs_here/${scene_name}_render_baseline.txt}"

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

    # test/stubs is the tree's one home for a host stand-in of an IDF
    # header, shared with check_app_sources.sh: each declares exactly what
    # the real header is used for, so a scene reaching a call nothing has
    # stubbed fails at the link rather than compiling into something else.
    _rs_flags="-I $_rs_launcher/main -I $_rs_launcher/components/microui/include"
    _rs_flags="$_rs_flags -I $_rs_tools -I $_rs_launcher/test/stubs"
    for _rs_inc in $scene_includes; do
        _rs_flags="$_rs_flags -I $_rs_launcher/$_rs_inc"
    done

    _rs_files="$_rs_tools/render_host.c $_rs_tools/render_video.c"
    for _rs_src in $scene_sources; do
        _rs_files="$_rs_files $_rs_launcher/$_rs_src"
    done

    # -lm LAST, after the sources, because GNU ld resolves left to right and
    # would otherwise discard libm before seeing who needed it. The scroll
    # view's momentum reaches expf() and lroundf(); the Windows toolchains
    # this repo also builds on fold those into libc and link clean without
    # it, which is how a scene that needs them reached CI unlinked. Harmless
    # where libm is already part of libc - run_tests.sh ends its own link
    # line the same way.
    # shellcheck disable=SC2086
    "$_rs_cc" -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
        -Wno-unused-variable -O1 $_rs_flags $scene_defines $_rs_files -o "$_rs_bin" -lm || return 1

    _rs_log="$scene_out_dir/render.log"
    _rs_new="$scene_out_dir/baseline.new"
    : > "$_rs_new"
    echo "$scene_renders" | {
        while IFS= read -r _rs_line; do
            [ -n "$_rs_line" ] || continue
            _rs_label=${_rs_line%%|*}
            _rs_tail=${_rs_line#*|}
            _rs_args=${_rs_tail%%|*}
            _rs_want=${_rs_tail##*|}
            _rs_path="$scene_out_dir/$_rs_label.bmp"
            _rs_video_args=""
            if [ "$_rs_video" = 1 ]; then
                _rs_video_args="--video $scene_out_dir/$_rs_label.avi"
            fi

            # shellcheck disable=SC2086
            if ! "$_rs_bin" $_rs_args -o "$_rs_path" $_rs_video_args 2> "$_rs_log"; then
                cat "$_rs_log" >&2
                echo "FAIL $scene_name/$_rs_label: the renderer exited non-zero" >&2
                exit 1
            fi
            _rs_said=$(sed -n 's/^RENDER [^ ]* \([0-9]*x[0-9]*\).*/\1/p' "$_rs_log")
            if [ "$_rs_said" != "$_rs_want" ]; then
                echo "FAIL $scene_name/$_rs_label: wrote ${_rs_said:-nothing}, declared $_rs_want" >&2
                exit 1
            fi
            _rs_hash=$(render_scene_sha256 "$_rs_path")
            printf '%s %s\n' "$_rs_label" "$_rs_hash" >> "$_rs_new"

            _rs_pinned=""
            if [ "$scene_pin" = 1 ] && [ -f "$scene_baseline" ]; then
                _rs_pinned=$(awk -v l="$_rs_label" '$1 == l { print $2 }' "$scene_baseline")
            fi
            if [ "$scene_pin" != 1 ]; then
                echo "ok $scene_name/$_rs_label $_rs_want -> $_rs_path (size only, not pinned)"
            elif [ -z "$_rs_hash" ]; then
                echo "ok $scene_name/$_rs_label $_rs_want -> $_rs_path (no sha256 tool; not checked)"
            elif [ -z "$_rs_pinned" ]; then
                echo "ok $scene_name/$_rs_label $_rs_want -> $_rs_path (not pinned)"
            elif [ "$_rs_pinned" != "$_rs_hash" ] && [ "$_rs_repin" = 1 ]; then
                echo "ok $scene_name/$_rs_label $_rs_want -> $_rs_path (changed; re-pinning)"
            elif [ "$_rs_pinned" != "$_rs_hash" ]; then
                echo "FAIL $scene_name/$_rs_label: the pixels changed" >&2
                echo "  pinned $_rs_pinned" >&2
                echo "  now    $_rs_hash" >&2
                echo "  Look at $_rs_path. If the change is wanted, re-pin with" >&2
                echo "  $0 --update-baseline" >&2
                exit 1
            else
                echo "ok $scene_name/$_rs_label $_rs_want -> $_rs_path (pinned)"
            fi
        done
    } || { rm -f "$_rs_new"; return 1; }
    rm -f "$_rs_log"

    if [ "$_rs_repin" = 1 ] && [ "$scene_pin" != 1 ]; then
        echo "$scene_name declares scene_pin=0; nothing to re-pin"
    elif [ "$_rs_repin" = 1 ]; then
        # The command as it would be typed from the repository root, not as
        # this machine spells it: an absolute path here would differ per
        # checkout and churn the file.
        _rs_root=$(dirname "$_rs_launcher")
        _rs_self=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/$(basename "$0")
        {
            echo "# What the $scene_name scene's renders hash to, one per line."
            echo "# Re-pinned deliberately, after looking at the images:"
            echo "#     ./${_rs_self#"$_rs_root"/} --update-baseline"
            cat "$_rs_new"
        } > "$scene_baseline"
        echo "re-pinned $scene_baseline"
    fi
    rm -f "$_rs_new"

    # A .png beside each BMP when Python and Pillow happen to be installed.
    # Neither is a dependency, and nothing here installs one.
    if _rs_python=$(command -v python3 || command -v python); then
        "$_rs_python" "$(render_scene_to_native "$_rs_tools/render_png.py")" \
            "$(render_scene_to_native "$scene_out_dir")" || return 1
    fi
}
