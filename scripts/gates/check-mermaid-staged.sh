#!/bin/sh
#
# Validates the STAGED content of every staged Markdown file that has a
# ```mermaid fence, so a broken diagram never reaches a commit.
#
#   scripts/gates/check-mermaid-staged.sh
#
# Checks the staged blob, not the file on disk - same reasoning as
# check-format-staged.sh: `git show ":$file"` piped into check-mermaid.mjs's
# --stdin mode, one file at a time, so a partially staged file is judged by
# what is actually about to be committed.
#
# A missing node or mmdc skips this check with a warning instead of
# blocking the commit. An mmdc that IS on PATH but cannot find a Chrome
# still blocks: the probe below only proves the binary runs, not that it
# can render, so every block in the commit fails. Tools are probed by
# running them, since a stub on PATH can exist without working.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$REPO_ROOT"

staged=$(git diff --cached --name-only --diff-filter=ACMR -- '*.md')
[ -n "$staged" ] || exit 0

files=""
IFS='
'
for file in $staged; do
    if git show ":$file" | grep -q '^[[:space:]]*```[[:space:]]*mermaid[[:space:]]*$'; then
        files="$files $file"
    fi
done
unset IFS

[ -n "$files" ] || exit 0

if ! node --version >/dev/null 2>&1; then
    echo "pre-commit: mermaid validation skipped, node not found on PATH" >&2
    exit 0
fi

if ! mmdc --version >/dev/null 2>&1; then
    echo "pre-commit: mermaid validation skipped, mmdc (mermaid-cli) not found on PATH" >&2
    exit 0
fi

status=0
for file in $files; do
    if ! git show ":$file" | node scripts/gates/check-mermaid.mjs --stdin "$file"; then
        status=1
    fi
done

exit "$status"
