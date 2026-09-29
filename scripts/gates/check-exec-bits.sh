#!/bin/sh
#
# Fails when a tracked file that starts with a shebang is not stored as
# executable. Windows checkouts have no exec bit, so a script added there is
# mode 100644 unless someone runs `git update-index --chmod=+x`, and on Linux
# `./script.sh` then answers "Permission denied".
#
#   scripts/gates/check-exec-bits.sh
#
# A shebang says the file is a program, so a sourced library that carries one
# is held to it too.

set -eu

cd "$(git rev-parse --show-toplevel)"
bad=$(git ls-files -s | awk -F'\t' '$1 ~ /^100644/ && $2 !~ /^third_party\// {print $2}' |
    while IFS= read -r path; do
        if [ "$(head -c 2 -- "$path" 2>/dev/null)" = "#!" ]; then echo "$path"; fi
    done)
if [ -n "$bad" ]; then
    echo "tracked scripts that are not executable:" >&2
    echo "$bad" | sed 's/^/  /' >&2
    echo "fix: git update-index --chmod=+x <path>" >&2
    exit 1
fi
