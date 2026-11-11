#!/bin/bash
# optimzr-gentoo benchmark runner.
#
#   bench/run.sh <label> [suite...]
#   suites: cpu mem compile compress storage net gpu   (default: all but gpu)
#           levels idlegov gpucap eth   (settings sweeps for the Power Control profiles)
#   env: REPS=3  KEEP_SERVICES=1 (don't stop services)
#
# Each test is repeated REPS times; the median goes to results/<label>/summary.tsv
# together with average package power (RAPL), max Tctl and average clock of that test.
# Benchmark binaries in bench/bin are built once and reused, so before/after
# numbers measure the system, not a different benchmark build.
set -u
ROOT=$(cd "$(dirname "$0")" && pwd)
LABEL=${1:?usage: run.sh <label> [suite...]}
shift
SUITES=${*:-cpu mem compile compress storage net}
OUT=$ROOT/results/$LABEL
BIN=$ROOT/bin
WORK=$ROOT/work
REPS=${REPS:-3}
H="sudo -n optimzr-root"
NPROC=$(nproc)
mkdir -p "$OUT/logs" "$WORK"
SUM=$OUT/summary.tsv
[ -f "$SUM" ] || printf 'test\tmetric\tvalue\tunit\tbetter\tpkg_w\ttctl_max\tcpu_mhz\n' > "$SUM"

log() { printf '[%s] %s\n' "$(date +%T)" "$*" | tee -a "$OUT/run.log"; }

# ------------------------------------------------------------ services
# services to stop during runs: one per line in bench/services.local (not in git)
SERVICES=$(grep -v "^#" "$ROOT/services.local" 2>/dev/null | tr "\n" " ")
STOPPED=""
services_stop() {
    [ "${KEEP_SERVICES:-0}" = 1 ] && return
    for s in $SERVICES; do
        if $H svc "$s" status >/dev/null 2>&1; then
            log "stopping $s"
            $H svc "$s" stop >/dev/null 2>&1 && STOPPED="$s $STOPPED"
        fi
    done
    sleep 5
}
services_start() {
    for s in $STOPPED; do
        log "starting $s"
        # some daemons take a while to shut down; retry for up to a minute
        local i
        for i in 1 2 3 4 5 6 7; do
            $H svc "$s" start >/dev/null 2>&1 && break
            [ "$i" = 7 ] && log "WARNING: $s did not start" || sleep 10
        done
    done
    STOPPED=""
}
trap 'services_start; $H revert-runtime >/dev/null 2>&1; pkill -f "$BIN/sampler" 2>/dev/null' EXIT INT TERM

# ------------------------------------------------------------ environment
snapshot() {
    {
        echo "date $(date -Is)"
        echo "kernel $(uname -r)"
        echo "cmdline $(cat /proc/cmdline)"
        echo "governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
        echo "epp $(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference)"
        echo "boost $(cat /sys/devices/system/cpu/cpufreq/boost)"
        echo "thp $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
        echo "nvme_sched $(cat /sys/block/nvme0n1/queue/scheduler)"
        echo "gpu_perf $(cat /sys/class/drm/card1/device/power_dpm_force_performance_level)"
        echo "gpu_cap_w $(($(cat /sys/class/drm/card1/device/hwmon/hwmon*/power1_cap) / 1000000))"
        echo "tcp_cc $(sysctl -n net.ipv4.tcp_congestion_control) qdisc $(tc qdisc show dev enp10s0 | head -1)"
        echo "gcc $(gcc -dumpfullversion) clang $(clang -dumpversion)"
        echo "make.conf $(grep -E '^(COMMON_FLAGS|MAKEOPTS|LDFLAGS)=' /etc/portage/make.conf | tr '\n' ' ')"
    } > "$OUT/env.txt"
}

# ------------------------------------------------------------ framework
rapl_uj() { $H rapl 2>/dev/null | awk '$2=="package-0"{print $3}'; }
RAPL_MAX=$(cat /sys/class/powercap/intel-rapl:0/max_energy_range_uj 2>/dev/null || echo 0)

# bench <test> <function>: the function prints "RESULT metric value unit better" lines
bench() {
    local test=$1 fn=$2 r reps="$OUT/logs/$1.reps"
    : > "$reps"
    for r in $(seq 1 "$REPS"); do
        log "$test rep $r/$REPS"
        local slog="$OUT/logs/$test.$r.sensors.csv" e0 e1 t0 t1 ssum
        "$BIN/sampler" "$slog" 500 > "$OUT/logs/$test.$r.sensors.txt" &
        local sp=$!
        e0=$(rapl_uj); t0=$(date +%s.%N)
        "$fn" > "$OUT/logs/$test.$r.log" 2>&1
        t1=$(date +%s.%N); e1=$(rapl_uj)
        kill "$sp"; wait "$sp" 2>/dev/null
        ssum=$(cat "$OUT/logs/$test.$r.sensors.txt")
        local watts
        watts=$(awk -v a="$e0" -v b="$e1" -v m="$RAPL_MAX" -v t0="$t0" -v t1="$t1" \
            'BEGIN{d=b-a; if(d<0)d+=m; printf "%.1f", d/1e6/(t1-t0)}')
        local tmax mhz
        tmax=$(sed -n 's/.*tctl_max=\([0-9.]*\).*/\1/p' <<<"$ssum")
        mhz=$(sed -n 's/.*cpu_avg_mhz=\([0-9]*\).*/\1/p' <<<"$ssum")
        grep '^RESULT ' "$OUT/logs/$test.$r.log" | while read -r _ m v u b; do
            printf '%s %s %s %s %s %s %s\n' "$m" "$v" "$u" "$b" "$watts" "$tmax" "$mhz"
        done >> "$reps"
        grep -q '^RESULT ' "$OUT/logs/$test.$r.log" || log "WARNING: $test rep $r gave no result (see logs/$test.$r.log)"
        sleep 3
    done
    # median per metric; power/temp/clock taken from the median rep
    awk -v test="$test" '
        { k=$1; n[k]++; v[k,n[k]]=$2; row[k,n[k]]=$0; unit[k]=$3; better[k]=$4
          if (!(k in seen)) { seen[k]=1; order[++no]=k } }
        END {
          for (o=1;o<=no;o++) { k=order[o]; c=n[k]
            for (i=1;i<=c;i++) idx[i]=i
            for (i=1;i<=c;i++) for (j=i+1;j<=c;j++) if (v[k,idx[j]]+0 < v[k,idx[i]]+0) { t=idx[i]; idx[i]=idx[j]; idx[j]=t }
            m=idx[int((c+1)/2)]; split(row[k,m], f, " ")
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", test, k, f[2], unit[k], better[k], f[5], f[6], f[7] } }' \
        "$reps" >> "$SUM"
    grep -P "^$test\t" "$SUM" | tail -n +1 | cut -f2,3,4 | sed 's/^/    /' | tee -a "$OUT/run.log" >/dev/null
}

quiet() { "$@" >/dev/null; }
elapsed() { # elapsed <metric> <cmd...>: prints RESULT with seconds
    local m=$1 t0 t1; shift
    t0=$(date +%s.%N)
    "$@" || { echo "FAILED: $*"; return 1; }
    t1=$(date +%s.%N)
    echo "RESULT $m $(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}') s lower"
}

# ------------------------------------------------------------ CPU
t_coremark_mt() { "$BIN/coremark-32" | tee /dev/stderr | awk '/^CoreMark 1.0 :/{print "RESULT coremark_mt", $4, "it/s higher"}'; }
t_coremark_st() { taskset -c 0 "$BIN/coremark-1" | awk '/^CoreMark 1.0 :/{print "RESULT coremark_st", $4, "it/s higher"}'; }
t_7z_mt() { 7z b -mmt"$NPROC" | awk '/^Tot:/{print "RESULT 7z_mt", $4, "MIPS higher"}'; }
t_7z_st() { taskset -c 0 7z b -mmt1 | awk '/^Tot:/{print "RESULT 7z_st", $4, "MIPS higher"}'; }
t_sysbench_cpu() {
    sysbench cpu --threads="$NPROC" --time=20 --cpu-max-prime=50000 run | awk '/events per second/{print "RESULT sysbench_cpu_mt", $4, "ev/s higher"}'
    sysbench cpu --threads=1 --time=10 --cpu-max-prime=50000 run | awk '/events per second/{print "RESULT sysbench_cpu_st", $4, "ev/s higher"}'
}
t_stressng() {
    local y=$WORK/sng.yaml
    stress-ng --matrix "$NPROC" -t 30 --metrics --yaml "$y" >/dev/null 2>&1
    awk '/bogo-ops-per-second-real-time/{print "RESULT stressng_matrix", $2, "ops/s higher"; exit}' "$y"
    stress-ng --cpu "$NPROC" --cpu-method all -t 30 --metrics --yaml "$y" >/dev/null 2>&1
    awk '/bogo-ops-per-second-real-time/{print "RESULT stressng_cpu_all", $2, "ops/s higher"; exit}' "$y"
}
suite_cpu() {
    bench coremark_mt t_coremark_mt
    bench coremark_st t_coremark_st
    bench 7z_mt t_7z_mt
    bench 7z_st t_7z_st
    bench sysbench_cpu t_sysbench_cpu
    bench stressng t_stressng
}

# ------------------------------------------------------------ memory
t_membench() { "$BIN/membench" "$NPROC" | awk '{u=$3; b=(u=="ns")?"lower":"higher"; print "RESULT", $1, $2, u, b}'; }
t_sysbench_mem() {
    sysbench memory --threads="$NPROC" --memory-block-size=1M --memory-total-size=200G run | awk -F'[()]' '/MiB transferred/{split($2,a," "); print "RESULT sysbench_mem_write", a[1], "MiB/s higher"}'
}
suite_mem() {
    bench membench t_membench
    bench sysbench_mem t_sysbench_mem
}

# ------------------------------------------------------------ compile
KSRC=$WORK/linux-7.3.3
prep_kernel() { [ -d "$KSRC" ] || tar -C "$WORK" -xf "$ROOT/third_party/linux-7.3.3.tar.xz"; }
kbuild() { # kbuild <metric> [make vars...]
    local m=$1; shift
    local o=$WORK/kbuild-$m
    rm -rf "$o"
    make -s -C "$KSRC" O="$o" "$@" defconfig >/dev/null
    # 6.12 predates GCC 16 / Clang 22 warnings; -Werror would stop the build
    "$KSRC/scripts/config" --file "$o/.config" -d WERROR
    make -s -C "$KSRC" O="$o" "$@" olddefconfig >/dev/null
    elapsed "$m" quiet make -s -C "$KSRC" O="$o" "$@" -j"$NPROC"
    rm -rf "$o"
}
t_kbuild_gcc() { kbuild kernel_defconfig_gcc; }
t_kbuild_clang() { kbuild kernel_defconfig_clang LLVM=1; }
suite_compile() {
    prep_kernel
    bench kbuild_gcc t_kbuild_gcc
    bench kbuild_clang t_kbuild_clang
}

# ------------------------------------------------------------ compression
CORPUS=$WORK/corpus.tar
prep_corpus() { prep_kernel; [ -f "$CORPUS" ] || tar -C "$WORK" -cf "$CORPUS" linux-7.3.3; }
mbs() { awk -v s="$(stat -c %s "$CORPUS")" -v t="$1" 'BEGIN{printf "%.1f", s/1048576/t}'; }
timed() { local t0 t1; t0=$(date +%s.%N); "$@" || return 1; t1=$(date +%s.%N); awk -v a="$t0" -v b="$t1" 'BEGIN{print b-a}'; }
t_zstd() {
    local t
    t=$(timed sh -c "zstd -q -f -3 -T$NPROC '$CORPUS' -o '$WORK/c.zst'"); echo "RESULT zstd3_mt $(mbs "$t") MB/s higher"
    t=$(timed sh -c "zstd -q -f -19 -T$NPROC '$CORPUS' -o '$WORK/c19.zst'"); echo "RESULT zstd19_mt $(mbs "$t") MB/s higher"
    t=$(timed sh -c "zstd -q -d -f '$WORK/c.zst' -o /dev/null"); echo "RESULT zstd_decomp $(mbs "$t") MB/s higher"
    rm -f "$WORK/c.zst" "$WORK/c19.zst"
}
t_xz() {
    local t
    t=$(timed sh -c "xz -q -k -f -6 -T$NPROC -c '$CORPUS' > '$WORK/c.xz'"); echo "RESULT xz6_mt $(mbs "$t") MB/s higher"
    t=$(timed sh -c "xz -q -d -T$NPROC -c '$WORK/c.xz' > /dev/null"); echo "RESULT xz_decomp_mt $(mbs "$t") MB/s higher"
    rm -f "$WORK/c.xz"
}
suite_compress() {
    prep_corpus
    bench zstd t_zstd
    bench xz t_xz
}

# ------------------------------------------------------------ storage (root fs, O_DIRECT)
FIOF=$WORK/fio.dat
t_fio() {
    local common="--filename=$FIOF --size=8G --direct=1 --ioengine=psync --time_based --runtime=15 --group_reporting --output-format=terse"
    # terse v3: field 7 read KiB/s, 8 read IOPS, 48 write KiB/s, 49 write IOPS
    # Order matters on this DRAM-less SSD: reads of randomly written data are
    # ~10x slower (FTL map misses). So every rep starts from a freshly
    # sequential-written file, and the fragmented read is its own metric.
    rm -f "$FIOF"
    fio --name=sw $common --rw=write --bs=1M --numjobs=4 --ioengine=libaio --iodepth=32 | awk -F';' '{printf "RESULT seq_write %.0f MB/s higher\n", $48/1024}'
    fio --name=sr $common --rw=read --bs=1M --numjobs=4 --ioengine=libaio --iodepth=32 | awk -F';' '{printf "RESULT seq_read %.0f MB/s higher\n", $7/1024}'
    fio --name=r1 $common --rw=randread --bs=4k --numjobs=1 | awk -F';' '{print "RESULT rand4k_read_qd1", $8, "IOPS higher"}'
    fio --name=r64 $common --rw=randread --bs=4k --numjobs=8 --ioengine=libaio --iodepth=32 | awk -F';' '{print "RESULT rand4k_read_qd256", $8, "IOPS higher"}'
    fio --name=w32 $common --rw=randwrite --bs=4k --numjobs=4 --ioengine=libaio --iodepth=32 | awk -F';' '{print "RESULT rand4k_write_qd128", $49, "IOPS higher"}'
    fio --name=srf $common --rw=read --bs=1M --numjobs=4 --ioengine=libaio --iodepth=32 | awk -F';' '{printf "RESULT seq_read_fragmented %.0f MB/s higher\n", $7/1024}'
    rm -f "$FIOF"
}
suite_storage() {
    bench fio t_fio
}

# ------------------------------------------------------------ network (no LAN peer: gateway + WAN + loopback)
GW=$(ip route | awk '/^default/{print $3; exit}')
pingstats() { # pingstats <prefix> <ping args...>
    local p=$1; shift
    ping -q "$@" | awk -v p="$p" -F'[ /]' '
        /packet loss/ { for(i=1;i<=NF;i++) if ($i ~ /%$/) { sub("%","",$i); print "RESULT", p"_loss", $i, "% lower" } }
        /^rtt|^round-trip/ { print "RESULT", p"_avg", $8, "ms lower"; print "RESULT", p"_jitter", $10, "ms lower" }'
}
DLURL=https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.3.3.tar.xz
t_lan() { pingstats gw -c 200 -i 0.2 "$GW"; }
t_wan() {
    pingstats wan_idle -c 60 -i 0.2 1.1.1.1
    curl -s -o /dev/null -w '%{speed_download}\n' "$DLURL" | awk '{printf "RESULT wan_download %.1f Mbit/s higher\n", $1*8/1e6}'
    # latency while the line is saturated (bufferbloat)
    curl -s -o /dev/null "$DLURL" & local c=$!
    sleep 2
    pingstats wan_loaded -c 40 -i 0.2 1.1.1.1
    kill $c 2>/dev/null; wait $c 2>/dev/null
}
# iperf3 3.21 miscounts the receiver on loopback, netperf doesn't
t_loopback() {
    netserver -p 12866 >/dev/null 2>&1
    sleep 1
    netperf -P 0 -H 127.0.0.1 -p 12866 -l 10 -t TCP_STREAM | awk 'NF>=5{printf "RESULT loopback_tcp_stream %.0f Mbit/s higher\n", $5}'
    netperf -P 0 -H 127.0.0.1 -p 12866 -l 10 -t TCP_RR | awk 'NF>=6{printf "RESULT loopback_tcp_rr %.0f trans/s higher\n", $6; exit}'
    netperf -P 0 -H 127.0.0.1 -p 12866 -l 10 -t UDP_RR | awk 'NF>=6{printf "RESULT loopback_udp_rr %.0f trans/s higher\n", $6; exit}'
    pkill -f 'netserver -p 12866'
}
# what a kernel change can move: syscalls, scheduler, network stack, block layer
suite_kernel() {
    bench loopback t_loopback
    suite_storage
}
suite_net() {
    bench lan t_lan
    bench wan t_wan
    bench loopback t_loopback
}


# ------------------------------------------------------------ ethernet
# Settings behind the network profiles. Changed through the root helper and
# put back by revert-runtime (sysfs/sysctl) and the saved ethtool/tc state.
NIC=$(ip route show default | awk '{print $5; exit}')
pingdist() { # pingdist <prefix> <ping args...>: avg, p50, p99, jitter
    local p=$1; shift
    ping "$@" | awk -v p="$p" -F'time=' 'NF==2 { split($2, a, " "); v[++n]=a[1]; s+=a[1] }
        END { if (!n) exit; asort(v); m=s/n; for(i=1;i<=n;i++) d+=(v[i]-m)^2
              printf "RESULT %s_avg %.3f ms lower\nRESULT %s_p50 %.3f ms lower\n", p, m, p, v[int(n*0.5)]
              printf "RESULT %s_p99 %.3f ms lower\nRESULT %s_sd %.3f ms lower\n", p, v[int(n*0.99)], p, sqrt(d/n) }'
}
t_ethlat() { pingdist gw -c 400 -i 0.05 "$GW"; pingdist wan -c 100 -i 0.1 1.1.1.1; }
UPURL=https://speed.cloudflare.com/__up
t_ethload() {
    [ -f "$WORK/up.bin" ] || head -c 40M /dev/urandom > "$WORK/up.bin"
    curl -s -o /dev/null -w '%{speed_download}\n' "$DLURL" | awk '{printf "RESULT down %.1f Mbit/s higher\n", $1*8/1e6}'
    curl -s -o /dev/null "$DLURL" & local c=$!
    sleep 2; pingdist down_loaded -c 40 -i 0.2 1.1.1.1
    kill $c 2>/dev/null; wait $c 2>/dev/null
    curl -s -o /dev/null -w '%{speed_upload}\n' --data-binary @"$WORK/up.bin" "$UPURL" |
        awk '{printf "RESULT up %.1f Mbit/s higher\n", $1*8/1e6}'
    curl -s -o /dev/null --data-binary @"$WORK/up.bin" "$UPURL" & c=$!
    sleep 2; pingdist up_loaded -c 40 -i 0.2 1.1.1.1
    kill $c 2>/dev/null; wait $c 2>/dev/null
}
eth_set() { # eee(on/off) gro_ns defer cc qdisc
    $H ethtool --set-eee "$NIC" eee "$1" >/dev/null
    $H write /sys/class/net/$NIC/gro_flush_timeout "$2" >/dev/null
    $H write /sys/class/net/$NIC/napi_defer_hard_irqs "$3" >/dev/null
    $H sysctl net.ipv4.tcp_congestion_control "$4" >/dev/null
    $H tc replace "$NIC" "$5" >/dev/null
    # changing EEE renegotiates the link (~3 s down): wait for the router
    local i; for i in $(seq 1 40); do ping -c1 -W1 "$GW" >/dev/null 2>&1 && break; sleep 0.5; done
    log "eth: $(ethtool --show-eee $NIC 2>/dev/null | sed -n 's/.*EEE status: //p') gro=$(cat /sys/class/net/$NIC/gro_flush_timeout) defer=$(cat /sys/class/net/$NIC/napi_defer_hard_irqs) cc=$(sysctl -n net.ipv4.tcp_congestion_control) $(tc qdisc show dev $NIC | head -1 | cut -d' ' -f1-2)"
}
suite_eth() {
    eth_set on 20000 1 cubic fq_codel;   bench e_stock_lat t_ethlat
    eth_set off 20000 1 cubic fq_codel;  bench e_eeeoff_lat t_ethlat
    eth_set off 0 0 cubic fq_codel;      bench e_lowlat_lat t_ethlat
    eth_set on 20000 1 cubic fq_codel;   bench e_cubic_load t_ethload
    eth_set on 20000 1 bbr fq;           bench e_bbr_load t_ethload
    eth_set on 20000 1 cubic cake;       bench e_cake_load t_ethload
    eth_set on 20000 1 cubic fq_codel
    $H revert-runtime >/dev/null
}

# ------------------------------------------------------------ GPU
t_vkpeak() {
    # own sampler so GPU power/temperature land in the summary as metrics
    local s=$WORK/gpu-sensors.csv
    "$BIN/sampler" "$s" 250 > "$WORK/gpu-sensors.txt" & local sp=$!
    "$BIN/vkpeak" 0 | awk '
        $2=="=" { u=$4; m=$1; gsub("-","_",m)
          if (m ~ /^(fp32_scalar|fp32_vec4|fp16_vec4|fp16_matrix|fp64_scalar|int32_vec4|int8_dotprod|int8_matrix|bf16_matrix|copy_h2d|copy_d2h|copy_d2d)$/)
            print "RESULT vk_" m, $3, u, "higher" }'
    kill "$sp"; wait "$sp" 2>/dev/null
    # power and junction temperature while the GPU was busy (gpu_busy > 50)
    awk -F, 'NR>1 && $8>50 { n++; w+=$7; if ($6>j) j=$6 } END { if (n) { printf "RESULT gpu_busy_w_avg %.1f W lower\n", w/n; printf "RESULT gpu_junc_max %.1f C lower\n", j } }' "$s"
}
suite_gpu() { [ -x "$BIN/vkpeak" ] && bench vkpeak t_vkpeak || log "no bench/bin/vkpeak, skipping gpu"; }


# ------------------------------------------------------------ power levels
# The knobs behind the Power Control slider, measured one variant at a time
# through the root helper. Everything is put back with revert-runtime.
CPUF=/sys/devices/system/cpu
MAXKHZ=$(cat $CPUF/cpu0/cpufreq/cpuinfo_max_freq)
GPUD=/sys/class/drm/card1/device
GPUHW=$(echo $GPUD/hwmon/hwmon*)
cpu_set() { # governor epp boost max_khz
    $H write-all "$CPUF/cpu*/cpufreq/scaling_governor" "$1" >/dev/null
    [ "$1" = performance ] || $H write-all "$CPUF/cpu*/cpufreq/energy_performance_preference" "$2" >/dev/null
    $H write $CPUF/cpufreq/boost "$3" >/dev/null
    $H write-all "$CPUF/cpu*/cpufreq/scaling_max_freq" "$4" >/dev/null
    log "cpu: $(cat $CPUF/cpu0/cpufreq/scaling_governor) $(cat $CPUF/cpu0/cpufreq/energy_performance_preference) boost=$(cat $CPUF/cpufreq/boost) max=$(cat $CPUF/cpu0/cpufreq/scaling_max_freq)"
}
t_idle() { sleep 20; echo "RESULT idle 1 x higher"; }
t_rr() {
    netserver -p 12866 >/dev/null 2>&1; sleep 1
    netperf -P 0 -H 127.0.0.1 -p 12866 -l 10 -t TCP_RR | awk 'NF>=6{printf "RESULT tcp_rr %.0f trans/s higher\n", $6; exit}'
    pkill -f 'netserver -p 12866'
}
level_variant() { # name gov epp boost max_khz
    local n=$1; shift
    cpu_set "$@"
    bench "${n}_idle" t_idle
    bench "${n}_st" t_coremark_st
    bench "${n}_mt" t_coremark_mt
    bench "${n}_rr" t_rr
}
suite_levels() {
    level_variant L50 powersave power 0 "$MAXKHZ"
    level_variant L60 powersave balance_power 1 4500000
    level_variant L70 powersave balance_power 1 5200000
    level_variant L80 powersave balance_performance 1 "$MAXKHZ"
    level_variant L90 powersave performance 1 "$MAXKHZ"
    level_variant L100 performance performance 1 "$MAXKHZ"
    $H revert-runtime >/dev/null
}
suite_levels70() {
    level_variant L70a powersave balance_power 1 4600000
    level_variant L70b powersave balance_power 1 4800000
    cpu_set powersave balance_performance 1 "$MAXKHZ"
    $H revert-runtime >/dev/null
}
suite_idlegov() {
    local g
    cpu_set powersave balance_performance 1 "$MAXKHZ"
    for g in teo menu; do
        $H write $CPUF/cpuidle/current_governor $g >/dev/null
        log "cpuidle governor: $(cat $CPUF/cpuidle/current_governor)"
        bench "idle_${g}_idle" t_idle
        bench "idle_${g}_st" t_coremark_st
        bench "idle_${g}_rr" t_rr
    done
    $H revert-runtime >/dev/null
}
suite_gpucap() {
    local w
    for w in ${GPUCAPS:-222 254 285 317 340}; do
        $H write "$GPUHW/power1_cap" $((w * 1000000)) >/dev/null
        log "gpu cap $(($(cat $GPUHW/power1_cap) / 1000000)) W"
        bench "gpu${w}" t_vkpeak
    done
    $H write "$GPUHW/power1_cap" 317000000 >/dev/null
    $H write $GPUD/pp_power_profile_mode 1 >/dev/null
    log "gpu profile: $(grep '\*' $GPUD/pp_power_profile_mode)"
    bench gpu317_3d t_vkpeak
    $H revert-runtime >/dev/null
}

# ------------------------------------------------------------ main
log "label=$LABEL suites=[$SUITES] reps=$REPS kernel=$(uname -r)"
snapshot
services_stop
for s in $SUITES; do
    log "== suite $s"
    "suite_$s"
done
services_start
log "done -> $SUM"
column -t -s $'\t' "$SUM"
