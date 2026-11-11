#!/bin/sh
# Optional root helper for the optimzr-gentoo setup and benchmarks.
#
# Installs /usr/local/sbin/optimzr-root (root:root 0755) and a sudoers rule
# that lets your user run that one program without a password. Nothing else
# becomes passwordless. Every call is logged to /var/log/optimzr-root.log.
#
#   sudo ./setup.sh           install / update
#   sudo ./setup.sh --remove  take the access away again (backups are kept)
#
# Be aware: installing kernels, packages and boot config is root-level by
# nature. The helper stops mistakes (wrong path, wrong disk, wrong service),
# it is not a sandbox against someone who owns your user account.
set -eu

[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"

USER_NAME=${SUDO_USER:-}
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || { echo "run with sudo from your normal user"; exit 1; }

HERE=$(cd "$(dirname "$0")" && pwd)
PROJECT=$(dirname "$HERE")
BIN=/usr/local/sbin/optimzr-root
SUDOERS=/etc/sudoers.d/optimzr

if [ "${1:-}" = "--remove" ]; then
    rm -f "$SUDOERS" "$BIN"
    echo "removed $BIN and $SUDOERS (backups in /var/lib/optimzr kept)"
    exit 0
fi

# services the benchmarks may stop: one name per line in services.local
EXTRA=""
if [ -f "$HERE/services.local" ]; then
    for s in $(grep -v "^#" "$HERE/services.local"); do
        case $s in *[!a-z0-9_-]*) echo "bad service name $s"; exit 1 ;; esac
        EXTRA="$EXTRA\"$s\","
    done
fi

echo ">> building optimzr-root for $PROJECT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cc -O2 -Wall -Wextra -fstack-protector-strong -fPIE -pie \
   -DPROJECT_DIR="\"$PROJECT\"" -DEXTRA_SERVICES="$EXTRA" -o "$TMP/optimzr-root" "$HERE/optimzr-root.c"

install -o root -g root -m 0755 "$TMP/optimzr-root" "$BIN"
install -d -o root -g root -m 0700 /var/lib/optimzr
touch /var/log/optimzr-root.log && chmod 0644 /var/log/optimzr-root.log

printf '%s ALL=(root) NOPASSWD: %s\n' "$USER_NAME" "$BIN" > "$TMP/sudoers"
visudo -cf "$TMP/sudoers" >/dev/null
install -o root -g root -m 0440 "$TMP/sudoers" "$SUDOERS"

echo ">> done. test as $USER_NAME:  sudo -n optimzr-root status"
