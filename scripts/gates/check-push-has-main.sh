#!/bin/sh
# Read main without fetching: worktrees share remote-tracking ref locks.
set -eu
remote=$1
tracking_main=refs/remotes/$remote/main
main_sha=
status=0
while read -r _local_ref local_sha remote_ref _remote_sha; do
    case "$remote_ref" in
        refs/heads/main) continue ;;
        refs/heads/*) ;;
        *) continue ;;
    esac
    case "$local_sha" in
        *[!0]*) ;;
        *) continue ;;
    esac
    if git rev-parse --verify --quiet "$tracking_main" >/dev/null &&
        ! git merge-base "$tracking_main" "$local_sha" >/dev/null 2>&1; then
        continue
    fi
    if [ -z "$main_sha" ]; then
        if ! main_line=$(git ls-remote "$remote" refs/heads/main 2>/dev/null) || [ -z "$main_line" ]; then
            echo "main could not be read from $remote; to push anyway: git push --no-verify" >&2
            exit 1
        fi
        main_sha=${main_line%%[[:space:]]*}
    fi
    if ! git merge-base --is-ancestor "$main_sha" "$local_sha" 2>/dev/null; then
        printf '%s does not contain %s/main %.7s; run git fetch %s && git merge %s/main, then push again.\n' \
            "$remote_ref" "$remote" "$main_sha" "$remote" "$remote" >&2
        status=1
    fi
done
exit "$status"
