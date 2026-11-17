#!/bin/sh
#
# The first-party packages under launcher/packages/, for a host build that
# compiles firmware code without CMake. Each package is one folder
# holding include/, src/ and tests/; found by globbing, so adding a package
# changes no script.
#
# Usage: source this file and pass the launcher directory:
#
#   . "$LAUNCHER_DIR/tools/build/packages.sh"
#   "$CC_BIN" -I "$MAIN_DIR" $(package_includes "$LAUNCHER_DIR") \
#       $(package_sources "$LAUNCHER_DIR") ...
#
# package_include_dirs prints each package's include root, package_includes
# the same as "-I <root>" flags, package_sources every .c under a package's
# src/, package_suites every tests/ and bench/suite_*.c. package_requires prints the
# packages one package's CMakeLists.txt sets in `set(<name>_requires ...)`,
# package_reachable that package's folder and every package it reaches
# through them (package_requires.py's requires() and reachable()).

package_include_dirs() {
    for package_dir in "$1"/packages/*/; do
        [ -d "${package_dir}include" ] && printf '%s\n' "${package_dir%/}/include"
    done
    return 0
}

package_includes() {
    for include_dir in $(package_include_dirs "$1"); do
        printf -- '-I %s ' "$include_dir"
    done
}

testkit_includes() {
    for testkit_dir in $(package_reachable "$1/packages/testkit"); do
        [ -d "$testkit_dir/include" ] && printf -- '-I %s ' "$testkit_dir/include"
    done
    printf -- '-I %s/packages/testkit/unity ' "$1"
}

package_sources() {
    find "$1"/packages/*/src -name '*.c' 2>/dev/null | sort
}

package_suites() {
    find "$1"/packages/*/tests "$1"/packages/*/bench -name 'suite_*.c' 2>/dev/null | sort
}

package_requires() { # package-dir
    requires_name=$(basename "$1")
    [ -f "$1/CMakeLists.txt" ] || return 0
    for requires_dep in $(sed -n "s/^set(${requires_name}_requires\([[:space:]][^)]*\)\{0,1\}).*/\1/p" \
        "$1/CMakeLists.txt" | head -n 1); do
        printf '%s\n' "$requires_dep"
    done
}

package_reachable() { # package-dir
    reach_dir=$(CDPATH= cd -- "$1" && pwd)
    reach_seen=$reach_dir
    reach_todo=$reach_dir
    while [ -n "$reach_todo" ]; do
        reach_next=""
        for reach_current in $reach_todo; do
            for reach_name in $(package_requires "$reach_current"); do
                reach_dep=$(dirname "$reach_dir")/$reach_name
                [ -d "$reach_dep" ] || continue
                case " $reach_seen " in *" $reach_dep "*) continue ;; esac
                reach_seen="$reach_seen $reach_dep"
                reach_next="$reach_next $reach_dep"
            done
        done
        reach_todo=$reach_next
    done
    printf '%s\n' $reach_seen
}

app_directories() {
    for app_dir in "$1"/main/apps/*/ "$1"/main/shell/apps/*/; do
        [ -d "$app_dir" ] && printf '%s\n' "$app_dir"
    done
    return 0
}
