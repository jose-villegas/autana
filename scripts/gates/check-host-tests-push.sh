#!/bin/sh
# Git supplies every ref update on stdin; test once if any range changes C.
set -eu
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
zero=0000000000000000000000000000000000000000
empty=$(git hash-object -t tree --stdin </dev/null)
needed=0
while read -r _local_ref local_sha _remote_ref remote_sha; do
    [ -n "$local_sha" ] || continue
    [ "$local_sha" != "$zero" ] || continue
    if [ "$remote_sha" = "$zero" ]; then
        # A new branch counts from where it leaves main; one sharing no history with main counts every file it holds.
        remote_sha=$(git merge-base origin/main "$local_sha" 2>/dev/null) || remote_sha=$empty
    fi
    if ! changed=$(git diff --name-only "$remote_sha" "$local_sha" -- '*.c' '*.h'); then
        echo 'Cannot inspect pushed range. To skip: git push --no-verify' >&2
        exit 1
    fi
    [ -z "$changed" ] || needed=1
done
[ "$needed" = 1 ] || exit 0
if ! bash "$ROOT/launcher/test/run_tests.sh" --sanitize; then
    echo 'Sanitized host tests failed; fix them before pushing. To skip: git push --no-verify' >&2
    exit 1
fi
