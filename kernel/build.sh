#!/bin/sh
# Build the gentoo-power kernel (clang ThinLTO, optional AutoFDO profile).
#
#   ./build.sh                      stage 1: no profile
#   ./build.sh profile/<x>.afdo     stage 2: use an AutoFDO profile
#   ./build.sh --install [...]      build, then install through optimzr-root
#
# Output: src/linux-gentoo-power (in-tree build), log in build/.
# Installing adds a GRUB entry; it does not change the default entry
# (see docs/kernel.md).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=$here/src/linux-gentoo-power
install=0
profile=
for a in "$@"; do
	case $a in
	--install) install=1 ;;
	*) profile=$(realpath "$a") ;;
	esac
done
[ -d "$src" ] || { echo "run ./prepare.sh first" >&2; exit 1; }
[ -z "$profile" ] || [ -s "$profile" ] || { echo "profile $profile missing" >&2; exit 1; }

mkdir -p "$here/build"
log=$here/build/build-$(date +%Y%m%d-%H%M%S).log
mk() { make -C "$src" LLVM=1 -j"$(nproc)" "$@"; }

steps() {
	echo "== gentoo-power build $(date -Is)"
	echo "profile: ${profile:-none}"
	clang --version | head -1
	ld.lld --version

	cp "$here/config/base-7.3.0-tux.config" "$src/.config"
	grep -v '^#' "$here/config/gentoo-power.fragment" | grep . |
		while read -r opt; do
			# shellcheck disable=SC2086
			"$src/scripts/config" --file "$src/.config" $opt
		done
	lv=-gentoo-power-train
	[ -z "$profile" ] || lv=-gentoo-power
	"$src/scripts/config" --file "$src/.config" --set-str LOCALVERSION "$lv"
	mk olddefconfig >/dev/null || return 1
	for o in LOCALVERSION AUTOFDO_CLANG LTO_CLANG_THIN SCHED_BORE; do
		grep "^CONFIG_$o=" "$src/.config" || echo "CONFIG_$o not set"
	done

	start=$(date +%s)
	if [ -n "$profile" ]; then
		mk CLANG_AUTOFDO_PROFILE="$profile" || return 1
	else
		mk || return 1
	fi
	echo "built $(cat "$src/include/config/kernel.release") in $(($(date +%s) - start)) s"
	rel=$(cat "$src/include/config/kernel.release")
	cp "$src/.config" "$here/config/$rel.config"
	# profile.sh needs the exact vmlinux of the kernel it runs on
	cp "$src/vmlinux" "$here/build/vmlinux-$rel"
}

echo "building, log: $log"
steps > "$log" 2>&1 || { tail -40 "$log"; echo "build failed, see $log" >&2; exit 1; }
grep -E "^(built|CONFIG_)" "$log"

if [ $install = 1 ]; then
	sudo -n /usr/local/sbin/optimzr-root kernel-install "$src" >> "$log" 2>&1 ||
		{ tail -20 "$log"; echo "install failed, see $log" >&2; exit 1; }
	echo "installed $(cat "$src/include/config/kernel.release")"
fi
