#!/bin/sh
# Prints true when a workflow run of this repository uploaded an artifact named NAME that has not
# expired, else false. A run records what it was made from this way (doc-images-gpu.yml).
# Needs gh, GITHUB_REPOSITORY and a GH_TOKEN that can read actions.
set -eu
if [ $# -ne 1 ]; then
    echo "usage: $0 ARTIFACT_NAME" >&2
    exit 2
fi
count=$(gh api "repos/$GITHUB_REPOSITORY/actions/artifacts?name=$1" \
    --jq '[.artifacts[] | select(.expired | not)] | length')
if [ "$count" -gt 0 ]; then
    echo true
else
    echo false
fi
