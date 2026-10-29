#!/bin/sh

# Variant build helper, sourced after idf.sh: idf_variant_build <launcher-dir>
# <release|dev|diag> <build-dir> [options], <build-dir> the folder
# idf_variant_prepare returned. A build directory keeps its own
# sdkconfig, so remove one made from stale or different defaults before it
# silently builds the wrong image; assert the selected symbols afterward.

# idf_variant_prepare <launcher-dir> <build-name> [prepared-dir]: sets
# IDF_VARIANT_BUILD to the real build folder, linked from
# <launcher-dir>/<build-name> (build_paths.py); with prepared-dir it only
# resets that folder if it was configured for another ESP-IDF. Run it before
# logs or locks create an in-tree folder.
idf_variant_prepare() {
    if [ -n "${3:-}" ]; then
        IDF_VARIANT_BUILD=$3
        "$_IDF_PYTHON" "$_IDF_TOOLS/build_paths.py" reset "$1" "$3" || return 1
    else
        IDF_VARIANT_BUILD=$("$_IDF_PYTHON" "$_IDF_TOOLS/build_paths.py" prepare "$1" "$2") || return 1
    fi
}

idf_variant_disagreement() {
    config=$1
    for flag in $IDF_VARIANT_REQUIRED; do
        if ! grep -q "^${flag}=y" "$config"; then
            echo "$flag is not set"
            return 0
        fi
    done
    for flag in $IDF_VARIANT_FORBIDDEN; do
        if grep -q "^${flag}=y" "$config"; then
            echo "$flag is set"
            return 0
        fi
    done
    return 1
}

# idf_variant_build <launcher-dir> <release|dev|diag> <build-dir> [options]
# Options: --autorun, --perf-scope, --frame-laps (after --perf-scope), --qemu, --layout-seed <n>, --defaults <fragment> <symbol>,
# --hot-tunables, --configure (reconfigure only: sdkconfig and compile_commands.json,
# no compile).
idf_variant_build() {
    launcher_dir=$1
    variant=$2
    build_dir=$3
    shift 3

    IDF_VARIANT_DEFAULTS=sdkconfig.defaults
    IDF_VARIANT_REQUIRED=""
    IDF_VARIANT_FORBIDDEN="CONFIG_LAUNCHER_DEVELOPMENT CONFIG_LAUNCHER_SELFTEST CONFIG_LAUNCHER_SELFTEST_AUTORUN CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF_LAPS CONFIG_LAUNCHER_QEMU"
    perf_scope=0
    scope_note="perf scope"
    layout_seed=0
    hot_tunables=0
    idf_verb=build

    case "$variant" in
        release) IDF_VARIANT_REQUIRED=CONFIG_LAUNCHER_RELEASE ;;
        dev)
            IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;sdkconfig.defaults.dev"
            IDF_VARIANT_REQUIRED=CONFIG_LAUNCHER_DEVELOPMENT
            IDF_VARIANT_FORBIDDEN="CONFIG_LAUNCHER_SELFTEST CONFIG_LAUNCHER_SELFTEST_AUTORUN CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF_LAPS CONFIG_LAUNCHER_QEMU"
            ;;
        diag)
            IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;sdkconfig.defaults.diag"
            IDF_VARIANT_REQUIRED="CONFIG_LAUNCHER_DEVELOPMENT CONFIG_LAUNCHER_SELFTEST"
            IDF_VARIANT_FORBIDDEN="CONFIG_LAUNCHER_SELFTEST_AUTORUN CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF_LAPS CONFIG_LAUNCHER_QEMU"
            ;;
        *)
            echo "unknown build variant: $variant" >&2
            return 2
            ;;
    esac

    while [ $# -gt 0 ]; do
        case "$1" in
            --autorun)
                [ "$variant" = diag ] || { echo "--autorun needs diag" >&2; return 2; }
                IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;sdkconfig.defaults.diag_autorun"
                IDF_VARIANT_REQUIRED="$IDF_VARIANT_REQUIRED CONFIG_LAUNCHER_SELFTEST_AUTORUN"
                IDF_VARIANT_FORBIDDEN=$(printf '%s\n' "$IDF_VARIANT_FORBIDDEN" | sed 's/CONFIG_LAUNCHER_SELFTEST_AUTORUN//')
                ;;
            --perf-scope)
                [ "$variant" = diag ] || { echo "--perf-scope needs diag" >&2; return 2; }
                IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;sdkconfig.defaults.diag_perf"
                IDF_VARIANT_REQUIRED="$IDF_VARIANT_REQUIRED CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF"
                IDF_VARIANT_FORBIDDEN=$(printf '%s\n' "$IDF_VARIANT_FORBIDDEN" | sed 's/CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF //')
                perf_scope=1
                ;;
            --frame-laps)
                [ "$perf_scope" -eq 1 ] || { echo "--frame-laps needs --perf-scope before it" >&2; return 2; }
                IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;sdkconfig.defaults.diag_perf_laps"
                IDF_VARIANT_REQUIRED="$IDF_VARIANT_REQUIRED CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF_LAPS"
                IDF_VARIANT_FORBIDDEN=$(printf '%s\n' "$IDF_VARIANT_FORBIDDEN" | sed 's/CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF_LAPS //')
                scope_note="perf scope, frame laps"
                ;;
            --qemu)
                [ "$variant" = diag ] || { echo "--qemu needs diag" >&2; return 2; }
                IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;sdkconfig.defaults.qemu"
                IDF_VARIANT_REQUIRED="$IDF_VARIANT_REQUIRED CONFIG_LAUNCHER_QEMU"
                IDF_VARIANT_FORBIDDEN=$(printf '%s\n' "$IDF_VARIANT_FORBIDDEN" | sed 's/CONFIG_LAUNCHER_QEMU//')
                ;;
            --configure)
                idf_verb=reconfigure
                ;;
            --hot-tunables)
                [ "$variant" != release ] || { echo "--hot-tunables needs dev or diag" >&2; return 2; }
                hot_tunables=1
                ;;
            --layout-seed)
                [ $# -ge 2 ] || { echo "--layout-seed needs a number" >&2; return 2; }
                case "$2" in ''|*[!0-9]*) echo "--layout-seed needs a number, not $2" >&2; return 2 ;; esac
                layout_seed=$2
                shift
                ;;
            --defaults)
                [ $# -ge 3 ] || { echo "--defaults needs a fragment and symbol" >&2; return 2; }
                IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;$2"
                IDF_VARIANT_REQUIRED="$IDF_VARIANT_REQUIRED $3"
                shift 2
                ;;
            *)
                echo "unknown idf variant option: $1" >&2
                return 2
                ;;
        esac
        shift
    done

    config="$build_dir/sdkconfig"
    if [ -f "$config" ]; then
        stale=""
        for fragment in $(printf '%s' "$IDF_VARIANT_DEFAULTS" | tr ';' ' '); do
            if [ "$launcher_dir/$fragment" -nt "$config" ]; then
                stale="$fragment is newer"
                break
            fi
        done
        if [ -z "$stale" ]; then
            stale="$(idf_variant_disagreement "$config")" || stale=""
        fi
        if [ -n "$stale" ]; then
            echo "=== $build_dir/sdkconfig: $stale - regenerating it ==="
            rm -f "$config"
        fi
    fi

    banner=Building
    [ "$idf_verb" = build ] || banner=Configuring
    if [ "$perf_scope" -eq 1 ]; then
        echo "=== $banner $build_dir ($scope_note) ==="
    else
        echo "=== $banner $build_dir ==="
    fi
    # shellcheck disable=SC2086
    echo "=== $build_dir/sdkconfig must have:" $IDF_VARIANT_REQUIRED "==="
    # shellcheck disable=SC2086
    echo "===   and must not have:" $IDF_VARIANT_FORBIDDEN "==="
    if [ "$layout_seed" -ne 0 ]; then
        echo "=== layout seed $layout_seed ==="
    fi
    # Both values are required: SDKCONFIG keeps this variant's generated
    # config in its build directory while SDKCONFIG_DEFAULTS selects its seed.
    # The layout seed is always passed: the CMake cache would otherwise keep
    # the last seed a build directory was given.
    set -- -B "$build_dir" -D SDKCONFIG_DEFAULTS="$IDF_VARIANT_DEFAULTS" \
        -D LAUNCHER_LAYOUT_SEED="$layout_seed" -D TUNE_HOT_LIVE="$hot_tunables" \
        -D SDKCONFIG="$config"
    idf "$@" "$idf_verb" || return $?
    "$_IDF_PYTHON" "$_IDF_TOOLS/build_paths.py" stamp "$launcher_dir" "$build_dir" || return 1

    disagreement="$(idf_variant_disagreement "$config")" || disagreement=""
    if [ -n "$disagreement" ]; then
        echo "$build_dir/sdkconfig: $disagreement" >&2
        return 1
    fi
    if [ "$idf_verb" = build ] && [ ! -f "$build_dir/launcher.bin" ]; then
        echo "build reported success but produced no binary at $build_dir/launcher.bin" >&2
        return 1
    fi
}
