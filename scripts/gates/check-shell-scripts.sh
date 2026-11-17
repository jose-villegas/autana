#!/usr/bin/env bash
# Syntax-checks and lints every tracked sh or bash script of the repository it
# is run in: dash -n for #!/bin/sh, bash -n for bash, then shellcheck at
# error severity. Exits 1 on any failure.
#
#   scripts/gates/check-shell-scripts.sh            # list, then check
#   scripts/gates/check-shell-scripts.sh --list     # only print the scripts
#
# A shell script is what its first line says, not its name: tools/autana and
# the pre-commit hook have no .sh, and a glob by name never saw them. dash is
# a genuine POSIX parse; the same command run from Git Bash cannot fail this
# way, so CI runs this on Linux, where /bin/sh is dash.
#
# -S error to begin with: a floor, not a ceiling. shellcheck's style and info
# findings on scripts written before it was wired up are worth fixing, but not
# worth failing the build over on the day it lands; raise this to warning
# once they are cleared rather than leaving the gate permanently soft.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

scripts=()
while IFS= read -r -d '' f; do
    first=""
    IFS= read -r first < "$f" 2>/dev/null || true
    if [[ $first =~ ^#!.*[/[:space:]](ba)?sh([[:space:]]|$) ]]; then
        scripts+=("$f")
    fi
done < <(git ls-files -z)
printf '%s\n' "${scripts[@]}"
[ "${1:-}" = --list ] && exit 0
[ "${#scripts[@]}" -gt 0 ] || exit 0

status=0
for f in "${scripts[@]}"; do
    first=$(head -1 "$f")
    case "$first" in
        */bin/sh*) checker=dash ;;
        *bash*) checker=bash ;;
        *) continue ;;
    esac
    printf '%s -n %s ... ' "$checker" "$f"
    if "$checker" -n "$f"; then echo ok; else echo FAILED; status=1; fi
done

shellcheck -S error "${scripts[@]}" || status=1
exit "$status"
