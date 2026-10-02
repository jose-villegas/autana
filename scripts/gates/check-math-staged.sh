#!/bin/sh
#
# Validates the STAGED content of every staged Markdown file that has a `$`
# or a ```math fence, so a formula GitHub cannot render never reaches a commit.
#
#   scripts/gates/check-math-staged.sh
#
# Checks the staged blob, not the file on disk, as check-mermaid-staged.sh
# does: `git show ":$file"` piped into check-math.mjs's --stdin mode, one file
# at a time.
#
# A missing node, mathjax-full or markdown-it skips this check with a warning
# instead of blocking the commit; CI is what gates a formula.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$REPO_ROOT"

staged=$(git diff --cached --name-only --diff-filter=ACMR -- '*.md')
[ -n "$staged" ] || exit 0

files=""
IFS='
'
for file in $staged; do
    if git show ":$file" | grep -q -e '\$' -e '^[[:space:]]*```[[:space:]]*math[[:space:]]*$'; then
        files="$files
$file"
    fi
done
unset IFS

[ -n "$files" ] || exit 0

if ! node --version >/dev/null 2>&1; then
    echo "pre-commit: math validation skipped, node not found on PATH" >&2
    exit 0
fi

if ! node -e "import('./scripts/gates/check-math.mjs').then((m) => process.exit(m.loadDependencies() ? 0 : 1))" >/dev/null 2>&1; then
    echo "pre-commit: math validation skipped, mathjax-full or markdown-it not installed" >&2
    exit 0
fi

status=0
IFS='
'
for file in $files; do
    [ -n "$file" ] || continue
    if ! git show ":$file" | node scripts/gates/check-math.mjs --stdin "$file"; then
        status=1
    fi
done
unset IFS

exit "$status"
