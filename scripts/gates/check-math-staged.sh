#!/bin/sh
#
# Validates the STAGED content of every staged Markdown file, so no formula
# GitHub cannot render, formula written as code, or fence that never closes
# reaches a commit.
#
#   scripts/gates/check-math-staged.sh
#
# Checks the staged blob, not the file on disk, as check-mermaid-staged.sh
# does: `git show ":$file"` piped into check-math.mjs's --stdin mode, one file
# at a time.
#
# A missing node, or dependencies not installed at their pinned versions,
# skips this check with a warning instead of blocking the commit; CI is what
# gates a formula.

set -eu

# shellcheck disable=SC1007
GATE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$(git rev-parse --show-toplevel)"

staged=$(git diff --cached --name-only --diff-filter=ACMR -- '*.md')
[ -n "$staged" ] || exit 0

if ! node --version >/dev/null 2>&1; then
    echo "pre-commit: math validation skipped, node not found on PATH" >&2
    exit 0
fi

if ! node -e "import(require('url').pathToFileURL(process.argv[1])).then((m) => process.exit(m.dependencyProblem() ? 1 : 0))" "$GATE_DIR/check-math.mjs" >/dev/null 2>&1; then
    echo "pre-commit: math validation skipped, run npm ci --prefix scripts/gates" >&2
    exit 0
fi

status=0
IFS='
'
for file in $staged; do
    [ -n "$file" ] || continue
    if ! git show ":$file" | node "$GATE_DIR/check-math.mjs" --stdin "$file"; then
        status=1
    fi
done
unset IFS

exit "$status"
