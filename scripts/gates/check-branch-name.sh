#!/bin/sh

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [<name>]" >&2
    exit 2
fi

if [ "$#" -eq 1 ]; then
    name=$1
else
    name=$(git symbolic-ref --quiet --short HEAD) || name=
fi

case "$name" in
    main|__dolt_remote_info__) exit 0 ;;
esac

if printf '%s\n' "$name" | LC_ALL=C grep -Eq '^(feature|bugfix|hotfix)/[a-z0-9]+(-[a-z0-9]+)*$|^release/[a-z0-9]+([.-][a-z0-9]+)*$'; then
    exit 0
fi

echo "Invalid branch name '$name'. Use feature/, bugfix/, hotfix/, or release/ with lowercase letters and digits separated by single hyphens; dots are allowed only in release/." >&2
echo "Rename with: git branch -m <new-name>" >&2
exit 1
