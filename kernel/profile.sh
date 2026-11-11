#!/bin/sh
# Collect an AutoFDO profile on the running gentoo-power kernel, then turn it
# into profile/kernel.afdo for ./build.sh.
#
#   ./profile.sh [seconds per workload]      default 60
#
# Must run on the exact kernel that is in src/ (checked by build id). Uses
# perf with AMD LBRv2 branch records (event ex_ret_brn_tkn, kernel only).
# perf_event_paranoid and kptr_restrict are lowered for the run and put back.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
src=$here/src/linux-gentoo-power
H="sudo -n /usr/local/sbin/optimzr-root"
secs=${1:-60}
vmlinux=$here/build/vmlinux-$(uname -r)
out=$here/profile
mkdir -p "$out"

grep -q amd_lbr_v2 /proc/cpuinfo || { echo "CPU has no amd_lbr_v2" >&2; exit 1; }
[ -f "$vmlinux" ] || vmlinux=$src/vmlinux
want=$(od -An -tx1 -v /sys/kernel/notes | tr -d ' \n')
have=$(readelf -n "$vmlinux" | sed -n 's/.*Build ID: //p' | head -1)
case $want in *"$have"*) ;; *)
	echo "vmlinux build id $have does not match the running kernel" >&2
	exit 1 ;;
esac

old_par=$(cat /proc/sys/kernel/perf_event_paranoid)
old_kptr=$(cat /proc/sys/kernel/kptr_restrict)
restore() {
	$H sysctl kernel.perf_event_paranoid "$old_par" >/dev/null || true
	$H sysctl kernel.kptr_restrict "$old_kptr" >/dev/null || true
}
trap restore EXIT INT TERM
$H sysctl kernel.perf_event_paranoid -1 >/dev/null
$H sysctl kernel.kptr_restrict 0 >/dev/null

work=$root/bench/work
data=$out/perf-$(date +%Y%m%d-%H%M%S).data

# The load: what this machine does all day. Compiling (fork/exec/VFS/page
# cache), disk IO, TCP loopback, and the normal desktop and services in the
# background.
load() {
	n=$(nproc)
	if [ -d "$work/linux-7.3.3" ]; then
		k=$work/linux-7.3.3
		make -s -C "$k" mrproper >/dev/null 2>&1
		make -s -C "$k" defconfig >/dev/null 2>&1
		"$k/scripts/config" --file "$k/.config" -d WERROR
		make -s -C "$k" olddefconfig >/dev/null 2>&1
		timeout "$secs" make -s -C "$work/linux-7.3.3" -j"$n" >/dev/null 2>&1 || true
	fi
	if command -v fio >/dev/null; then
		fio --name=p --filename="$work/fio.prof" --size=2G --direct=1 --ioengine=io_uring \
			--rw=randrw --bs=4k --iodepth=32 --numjobs=4 --time_based --runtime="$secs" \
			--group_reporting >/dev/null 2>&1 || true
		fio --name=p --filename="$work/fio.prof" --size=2G --rw=read --bs=128k \
			--time_based --runtime=$((secs / 2)) >/dev/null 2>&1 || true
		rm -f "$work/fio.prof"
	fi
	if command -v netperf >/dev/null; then
		netserver -p 12867 >/dev/null 2>&1 || true
		netperf -p 12867 -H 127.0.0.1 -t TCP_STREAM -l $((secs / 2)) >/dev/null 2>&1 || true
		netperf -p 12867 -H 127.0.0.1 -t TCP_RR -l $((secs / 2)) >/dev/null 2>&1 || true
		pkill -f "netserver -p 12867" || true
	fi
	# small-file metadata churn
	timeout "$secs" sh -c 'while :; do find /usr/include -type f >/dev/null; \
		tar -cf - /usr/include 2>/dev/null | cat >/dev/null; done' || true
}

echo "recording (about $((secs * 4)) s)..."
perf record -q -e ex_ret_brn_tkn:k -a -N -b -c 500009 -o "$data" &
pid=$!
sleep 1
load
kill -INT $pid
wait $pid || true
ls -la "$data"
restore
trap - EXIT INT TERM

echo "converting..."
llvm-profgen --kernel --binary="$vmlinux" --perfdata="$data" -o "$out/kernel-$(uname -r).afdo"
cp "$out/kernel-$(uname -r).afdo" "$out/kernel.afdo"
ls -la "$out"/kernel*.afdo
echo "next: ./build.sh --install profile/kernel.afdo"
