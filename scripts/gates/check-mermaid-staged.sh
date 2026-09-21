#!/bin/sh
#
# Validates every staged Markdown file that has a ```mermaid fence, so a
# broken diagram never reaches a commit. GitHub is the only render target
# and it fails silently - a parse error where the picture should be, and
# nothing else catches it.
#
#   scripts/gates/check-mermaid-staged.sh
#
# Runs against the file on disk, not the staged blob: validate-mermaid.mjs
# reads a real file, and check-format-staged.sh's partial-stage concern does
# not apply the same way to a diagram, which is rarely split across hunks.
#
# A missing node or mmdc SKIPS this check with a warning instead of
# blocking the commit - nobody should have to install Puppeteer's Chrome
# just to commit an unrelated doc fix. Tools are probed by running them, not
# with `command -v`, for the same reason check-format-staged.sh does: a
# stub on PATH can exist without working.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$REPO_ROOT"

staged=$(git diff --cached --name-only --diff-filter=ACMR -- '*.md')
[ -n "$staged" ] || exit 0

files=""
IFS='
'
for file in $staged; do
    [ -f "$file" ] || continue
    if grep -q '^[[:space:]]*```[[:space:]]*mermaid[[:space:]]*$' "$file"; then
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

# shellcheck disable=SC2086  # word-splitting the staged file list is intended
exec node scripts/gates/validate-mermaid.mjs $files
