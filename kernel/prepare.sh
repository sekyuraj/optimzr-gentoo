#!/bin/sh
# Make kernel/src/linux-gentoo-power from a CachyOS-patched 7.3 tree plus the
# kernel.org 7.3.3 stable patch.
#
#   ./prepare.sh [base tree]     default base: /usr/src/linux-7.3.0-tux
#
# The base tree is CachyOS linux 7.3.0 with their BORE patch (what 7.3.0-tux
# was built from). Any vanilla 7.3 tree works too; you just lose the Cachy
# patches (BORE, ADIOS, ...) and must drop those options from the config.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
base=${1:-/usr/src/linux-7.3.0-tux}
stable=7.3.3
dst=$here/src/linux-gentoo-power

[ -f "$base/Makefile" ] || { echo "no kernel tree at $base" >&2; exit 1; }
[ -e "$dst" ] && { echo "$dst exists, remove it first" >&2; exit 1; }
mkdir -p "$here/src" "$here/dl"

# copy only source; build output and the old signing key stay behind
cp -a "$base" "$dst" 2>/dev/null || true
make -s -C "$dst" mrproper

cd "$here/dl"
if [ ! -f patch-$stable.xz ]; then
	curl -sSfLO https://cdn.kernel.org/pub/linux/kernel/v7.x/patch-$stable.xz
	curl -sSfL https://cdn.kernel.org/pub/linux/kernel/v7.x/sha256sums.asc |
		grep " patch-$stable.xz\$" > patch-$stable.sha256
fi
sha256sum -c patch-$stable.sha256

sub=$(sed -n 's/^SUBLEVEL = //p' "$dst/Makefile")
if [ "$sub" = 0 ]; then
	xzcat patch-$stable.xz | patch -d "$dst" -p1 -s
fi
find "$dst" \( -name '*.orig' -o -name '*.rej' \) -delete
echo "ready: $dst ($(make -s -C "$dst" kernelversion))"
