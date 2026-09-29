#!/bin/sh
#
# Pre-commit: refuses a staged script that starts with a shebang but is
# stored as mode 100644. A Windows author is who creates those, and the fix is
# one command the message prints. The same rule, over the whole tree, is
# check-exec-bits.sh.

set -eu

cd "$(git rev-parse --show-toplevel)"
bad=$(git diff --cached --raw --no-abbrev --diff-filter=AM |
    awk -F'\t' '$1 ~ /^:[0-9]+ 100644 / {print $2}' | tr '\n' '\0' |
    xargs -0 -r awk 'FNR==1 && /^#!/ {print FILENAME} {nextfile}')
if [ -n "$bad" ]; then
    echo "staged scripts that are not executable:" >&2
    echo "$bad" | sed 's/^/  /' >&2
    echo "fix: git update-index --chmod=+x <path>" >&2
    exit 1
fi
