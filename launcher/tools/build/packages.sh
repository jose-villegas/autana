#!/bin/sh
#
# The first-party packages under launcher/packages/ (math/ first), for a host
# build that compiles firmware code without CMake. Each package is one folder
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
# src/, package_suites every tests/suite_*.c.

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

package_sources() {
    find "$1"/packages/*/src -name '*.c' 2>/dev/null | sort
}

package_suites() {
    find "$1"/packages/*/tests -name 'suite_*.c' 2>/dev/null | sort
}
