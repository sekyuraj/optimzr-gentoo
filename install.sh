#!/bin/sh
# Install Gentoo Power Control (optimzrd + optimzr + optimzr-gui).
#
#   ./install.sh            build as you, then install (asks for sudo once)
#   ./install.sh --check    show what would be installed, change nothing
#
# The kernel (kernel/) and the Portage settings (portage/) are separate on
# purpose; see README.md. Undo with ./uninstall.sh.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
check=0
[ "${1:-}" = --check ] && check=1

say() { printf '%s\n' "$*"; }

# who gets to use the app
user=${SUDO_USER:-$(id -un)}

# How we become root: plain root, the optimzr-root helper (if this machine
# has it set up), or sudo.
if [ "$(id -u)" = 0 ]; then
	mode=root
elif sudo -n /usr/local/sbin/optimzr-root status >/dev/null 2>&1; then
	mode=helper
else
	mode=sudo
fi

if [ ! -x "$here/app/build/optimzrd" ] || [ "$here/app/src/optimzrd.c" -nt "$here/app/build/optimzrd" ] ||
	[ "$here/app/src/gui.c" -nt "$here/app/build/optimzr-gui" ]; then
	if [ "$(id -u)" = 0 ] && [ -n "${SUDO_USER:-}" ]; then
		su "$SUDO_USER" -c "make -C '$here/app'"
	else
		make -C "$here/app"
	fi
fi

if [ $check = 1 ]; then
	say "mode: $mode, user: $user"
	say "would install: /usr/local/sbin/optimzrd /usr/local/bin/optimzr /usr/local/bin/optimzr-gui"
	say "               /etc/init.d/optimzrd /etc/conf.d/optimzrd"
	say "               /usr/local/share/applications/optimzr-gui.desktop and icon"
	say "and enable optimzrd in the default runlevel"
	exit 0
fi

if [ $mode = sudo ]; then
	exec sudo SUDO_USER="$user" "$0" "$@"
fi

H="sudo -n /usr/local/sbin/optimzr-root"
inst() { # src dst mode
	if [ $mode = root ]; then
		install -D -m "$3" "$1" "$2"
	else
		$H install "$1" "$2" "$3" >/dev/null
	fi
	say "  $2"
}
svc() { if [ $mode = root ]; then rc-service "$@"; else $H svc "$@"; fi; }
rcupd() { if [ $mode = root ]; then rc-update "$@"; else $H rc-update "$@"; fi; }

# group: a dedicated one when we can create it, else wheel
group=optimzr
if [ $mode = root ]; then
	getent group optimzr >/dev/null || groupadd -r optimzr
	if [ "$user" != root ] && ! id -nG "$user" | tr ' ' '\n' | grep -qx optimzr; then
		usermod -aG optimzr "$user"
		say "added $user to group optimzr"
	fi
elif ! getent group optimzr >/dev/null; then
	group=wheel
fi

tmp=$(mktemp -d)
trap 'rm -f "$tmp"/*; rmdir "$tmp"' EXIT
sed "s/^OPTIMZR_GROUP=.*/OPTIMZR_GROUP=\"$group\"/" "$here/app/openrc/optimzrd.confd" > "$tmp/confd"

say "installing:"
svc optimzrd status >/dev/null 2>&1 && svc optimzrd stop >/dev/null 2>&1 || true
inst "$here/app/build/optimzrd" /usr/local/sbin/optimzrd 0755
inst "$here/app/build/optimzr" /usr/local/bin/optimzr 0755
inst "$here/app/build/optimzr-gui" /usr/local/bin/optimzr-gui 0755
inst "$here/app/openrc/optimzrd.initd" /etc/init.d/optimzrd 0755
inst "$tmp/confd" /etc/conf.d/optimzrd 0644
if [ $mode = root ] || [ -d /usr/local/share/applications ]; then
	inst "$here/app/data/optimzr-gui.desktop" /usr/local/share/applications/optimzr-gui.desktop 0644
	inst "$here/app/data/optimzr-gui.svg" /usr/local/share/icons/hicolor/scalable/apps/optimzr-gui.svg 0644
else
	# the helper doesn't create directories; a per-user entry works the same
	uhome=$(getent passwd "$user" | cut -d: -f6)
	install -D -m 0644 "$here/app/data/optimzr-gui.desktop" "$uhome/.local/share/applications/optimzr-gui.desktop"
	install -D -m 0644 "$here/app/data/optimzr-gui.svg" "$uhome/.local/share/icons/hicolor/scalable/apps/optimzr-gui.svg"
	say "  $uhome/.local/share/applications/optimzr-gui.desktop"
fi

# fan sensors: Gigabyte/ITE boards need it87 loaded (read-only use)
if grep -qi gigabyte /sys/class/dmi/id/board_vendor 2>/dev/null &&
	! ls /sys/class/hwmon/*/name 2>/dev/null | xargs cat 2>/dev/null | grep -q '^it8'; then
	if modinfo it87 >/dev/null 2>&1; then
		printf 'it87\n' > "$tmp/it87"
		inst "$tmp/it87" /etc/modules-load.d/optimzr-sensors.conf 0644
		if [ $mode = root ]; then modprobe it87 || true; else $H modprobe it87 >/dev/null || true; fi
	fi
fi

rcupd add optimzrd default >/dev/null 2>&1 || true
svc optimzrd start >/dev/null
say "optimzrd running; group: $group"
say "open \"Gentoo Power Control\" from your launcher, or run: optimzr"
