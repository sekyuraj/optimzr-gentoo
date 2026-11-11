#!/bin/sh
# Remove Gentoo Power Control. Stopping optimzrd first puts every setting it
# changed back to what it was.
set -eu
if [ "$(id -u)" = 0 ]; then
	mode=root
elif sudo -n /usr/local/sbin/optimzr-root status >/dev/null 2>&1; then
	mode=helper
else
	exec sudo "$0" "$@"
fi
H="sudo -n /usr/local/sbin/optimzr-root"
svc() { if [ $mode = root ]; then rc-service "$@"; else $H svc "$@"; fi; }
rcupd() { if [ $mode = root ]; then rc-update "$@"; else $H rc-update "$@"; fi; }
rm1() {
	[ -e "$1" ] || return 0
	case $1 in "$HOME"/*) rm -f "$1" ;; *) if [ $mode = root ]; then rm -f "$1"; else $H remove "$1" >/dev/null; fi ;; esac
	echo "  removed $1"
}

svc optimzrd stop >/dev/null 2>&1 || true
rcupd del optimzrd default >/dev/null 2>&1 || true
for f in /usr/local/sbin/optimzrd /usr/local/bin/optimzr /usr/local/bin/optimzr-gui \
	/etc/init.d/optimzrd /etc/conf.d/optimzrd /etc/modules-load.d/optimzr-sensors.conf \
	/usr/local/share/applications/optimzr-gui.desktop \
	/usr/local/share/icons/hicolor/scalable/apps/optimzr-gui.svg \
	"$HOME/.local/share/applications/optimzr-gui.desktop" \
	"$HOME/.local/share/icons/hicolor/scalable/apps/optimzr-gui.svg"; do
	rm1 "$f"
done
echo "done. State in /var/lib/optimzr and the log /var/log/optimzrd.log were kept."
