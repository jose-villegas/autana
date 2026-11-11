#!/bin/sh
# Installs Debian packages on a CI runner so that one package-mirror outage
# does not fail the job: apt retries each download, and when the runner's
# mirror still fails the index update, the sources move to FALLBACK_MIRROR and
# the update runs again. Run as root: sudo sh scripts/ci/apt_install.sh <apt-get install args...>
set -eu
APT_RETRIES=5
FALLBACK_MIRROR=http://archive.ubuntu.com/ubuntu
# Sources the runner images use: the deb822 file (24.04) and the one-line list (22.04).
SOURCES="/etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list"
APT_DIR=${APT_DIR:-}

apt() {
    apt-get -o Acquire::Retries="$APT_RETRIES" "$@"
}

if ! apt update; then
    echo "apt_install: index update failed; moving the sources to $FALLBACK_MIRROR" >&2
    for file in $SOURCES; do
        [ -f "$APT_DIR$file" ] || continue
        sed -E -i "s#https?://[^ ]*(archive|azure)[^ ]*ubuntu\.com/ubuntu/?#$FALLBACK_MIRROR/#g" "$APT_DIR$file"
    done
    apt update
fi
apt install --fix-missing "$@"
