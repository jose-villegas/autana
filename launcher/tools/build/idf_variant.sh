#!/bin/sh

# Variant build helper, sourced after idf.sh: idf_variant_build <launcher-dir>
# <release|dev|diag> <build-dir> [options]. A build directory keeps its own
# sdkconfig, so remove one made from stale or different defaults before it
# silently builds the wrong image; assert the selected symbols afterward.

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
# Options: --autorun, --perf-scope, --qemu, --defaults <fragment> <symbol>.
idf_variant_build() {
    launcher_dir=$1
    variant=$2
    build_dir=$3
    shift 3

    IDF_VARIANT_DEFAULTS=sdkconfig.defaults
    IDF_VARIANT_REQUIRED=""
    IDF_VARIANT_FORBIDDEN="CONFIG_LAUNCHER_DEVELOPMENT CONFIG_LAUNCHER_SELFTEST CONFIG_LAUNCHER_SELFTEST_AUTORUN CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF CONFIG_LAUNCHER_QEMU"
    perf_scope=0

    case "$variant" in
        release) IDF_VARIANT_REQUIRED=CONFIG_LAUNCHER_RELEASE ;;
        dev)
            IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;sdkconfig.defaults.dev"
            IDF_VARIANT_REQUIRED=CONFIG_LAUNCHER_DEVELOPMENT
            IDF_VARIANT_FORBIDDEN="CONFIG_LAUNCHER_SELFTEST CONFIG_LAUNCHER_SELFTEST_AUTORUN CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF CONFIG_LAUNCHER_QEMU"
            ;;
        diag)
            IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;sdkconfig.defaults.diag"
            IDF_VARIANT_REQUIRED="CONFIG_LAUNCHER_DEVELOPMENT CONFIG_LAUNCHER_SELFTEST"
            IDF_VARIANT_FORBIDDEN="CONFIG_LAUNCHER_SELFTEST_AUTORUN CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF CONFIG_LAUNCHER_QEMU"
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
                IDF_VARIANT_FORBIDDEN=$(printf '%s\n' "$IDF_VARIANT_FORBIDDEN" | sed 's/CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF//')
                perf_scope=1
                ;;
            --qemu)
                [ "$variant" = diag ] || { echo "--qemu needs diag" >&2; return 2; }
                IDF_VARIANT_DEFAULTS="$IDF_VARIANT_DEFAULTS;sdkconfig.defaults.qemu"
                IDF_VARIANT_REQUIRED="$IDF_VARIANT_REQUIRED CONFIG_LAUNCHER_QEMU"
                IDF_VARIANT_FORBIDDEN=$(printf '%s\n' "$IDF_VARIANT_FORBIDDEN" | sed 's/CONFIG_LAUNCHER_QEMU//')
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

    config="$launcher_dir/$build_dir/sdkconfig"
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

    if [ "$perf_scope" -eq 1 ]; then
        echo "=== Building $build_dir (perf scope) ==="
    else
        echo "=== Building $build_dir ==="
    fi
    # shellcheck disable=SC2086
    echo "=== $build_dir/sdkconfig must have:" $IDF_VARIANT_REQUIRED "==="
    # shellcheck disable=SC2086
    echo "===   and must not have:" $IDF_VARIANT_FORBIDDEN "==="
    # Both values are required: SDKCONFIG keeps this variant's generated
    # config in its build directory while SDKCONFIG_DEFAULTS selects its seed.
    idf -B "$build_dir" -D SDKCONFIG_DEFAULTS="$IDF_VARIANT_DEFAULTS" \
        -D SDKCONFIG="$build_dir/sdkconfig" build || return $?

    disagreement="$(idf_variant_disagreement "$config")" || disagreement=""
    if [ -n "$disagreement" ]; then
        echo "$build_dir/sdkconfig: $disagreement" >&2
        return 1
    fi
    if [ ! -f "$launcher_dir/$build_dir/launcher.bin" ]; then
        echo "build reported success but produced no binary at $build_dir/launcher.bin" >&2
        return 1
    fi
}
