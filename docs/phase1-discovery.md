# optimzr-gentoo · Phase 1 – Discovery (2026-11-10)

Read-only survey. Nothing on the system was changed.

## Hardware

| Part | Found | Notes |
|---|---|---|
| CPU | Ryzen 9 9950X, Zen 5 (fam 26 / model 68), 16C/32T, 2 CCDs | microcode 0xb404035, full AVX-512 incl. VP2INTERSECT, BF16, VNNI |
| Caches | 48K L1d + 32K L1i + 1M L2 per core, 2×32M L3 (one per CCD) | NUMA: 1 node; CCD0 = cpus 0-7,16-23, CCD1 = 8-15,24-31 |
| Boost | amd-pstate-epp active, prefcore on, max 5756 MHz | best cores by CPPC rank: 0,1,2,5 (CCD0) |
| All-core | ~5.1 GHz sustained, Tctl ~75 °C in 7-Zip | 20 °C below Tjmax: thermal headroom exists |
| Board | Gigabyte B650 GAMING X AX V2, BIOS F38 (2025-12-17) | PBO / Curve Optimizer / EXPO are BIOS-only |
| RAM | 2×16 GB Corsair CMG32GX5M2E6000Z36 (DDR5, EXPO 6000 CL36) | JEDEC base 4800. **Running speed unknown** (needs root dmidecode) |
| GPU | RX 9070-series, Navi 48, 16 GB, PCIe 4.0 x16 (board max) | power cap 317 W default, 340 W max; OverDrive **off** (ppfeaturemask 0xfff7bfff) |
| iGPU | Granite Ridge Radeon | bound to vfio-pci (kept for VMs) |
| System disk | SIX X7400 1 TB, PCIe 4.0 x4, ext4 noatime | mq-deadline scheduler |
| Other disks | 2× Netac NV3000 500 GB, PCIe 3.0 x4, DRAM-less | other OSes, not touched |
| Fans | no Super I/O driver loaded | it87.ko exists (IT8689E on this board); needs a root probe |
| Sensors | k10temp, amdgpu, gigabyte_wmi, spd5118 ×2, nvme ×3 | CPU power: RAPL package-0 (root-only read) |

## Software

| Area | Current state |
|---|---|
| Kernel | 7.3.0-tux: CachyOS 7.3 + BORE, clang ThinLTO, -O3, X86_NATIVE_CPU, HZ 1000, PREEMPT_DYNAMIC, NO_HZ_FULL, sched_ext, ADIOS available |
| Fallback | 6.18.57-gentoo-dist-bin in GRUB |
| Init | OpenRC (not systemd) |
| Idle | acpi_idle + menu governor, C1/C2/C3 (C3 exit 350 µs) |
| Memory | THP always, zswap zstd on, 4 GB swap, swappiness 100, max_map_count 65530 |
| Toolchain | GCC 16.2.1 (**built without pgo/lto**), Clang/LLD 22.1.8 (plain -O2), binutils 2.47 |
| Portage | -march=native -O2, MAKEOPTS -j32 -l32, --jobs=4, heavy.conf -j14 for big C++; no ccache, no mold, PORTAGE_TMPDIR on disk |
| Profile | default/linux/amd64/23.0 (non-desktop) |
| Load | public services run 24/7 (a web server, a Bitcoin node, game servers) |
| Missing tools | perf, turbostat, cpupower, lm-sensors, stress-ng, sysbench, fio, hyperfine, PTS, smartctl, nvme-cli, dmidecode, vkmark |

First numbers (sample only, services running): 7-Zip `b -mmt32` = 167,317 MIPS total.

## What's already tuned

Most kernel-side wins are already in: BORE, LTO, native march, 1000 Hz, amd-pstate-epp with prefcore. A gentoo-power kernel has to beat 7.3.0-tux, not a stock kernel.

## Headroom found, by expected size

1. **RAM at 4800 instead of 6000** (if EXPO is off). Memory-bound work could gain 10–25%. One BIOS toggle. Check first.
2. **Compile workflow.** ccache gives 5–20× on rebuilds. mold links big C++ 2–5× faster. GCC with USE="pgo lto" is a ~10% faster compiler, and Clang built with PGO+ThinLTO(+BOLT) runs ~25–35% faster. A size-capped tmpfs PORTAGE_TMPDIR adds a few percent. This is where 2× is real.
3. **CPU power.** PBO plus a per-core negative Curve Optimizer (BIOS) gives ~5–10% multi-thread with the current 20 °C headroom. A runtime EPP/governor switch helps latency and bursty single-thread work.
4. **GPU.** amdgpu OverDrive (ppfeaturemask) opens the official fan-curve, clock-offset and voltage-offset interfaces. Power cap 317 → 340 W is +7%. 3D_FULL_SCREEN power profile for games.
5. **Kernel.** Clang AutoFDO (Zen 5 has LBRv2) on top of ThinLTO, plus a trimmed config and the teo idle governor. Expect a few percent on syscall/IO-heavy work. Measure everything.
6. **Storage.** NVMe scheduler none vs mq-deadline, read-ahead. Small, measured with fio.
7. **Small fixes.** vm.max_map_count for games; split benchmarks away from the 24/7 services.

Not planned: mitigations=off (this box serves the public internet; Zen 5 mitigations cost ~1–3%), ryzen_smu or any SMU/MSR writes, forced voltages.

## Plan

- **Phase 2 baseline:** install the benchmark tools (one script). Stop services during runs. Run 7-Zip, CoreMark, stress-ng, sysbench mem, fio, kernel compile (defconfig + our config), LLVM compile, zstd/xz, vkmark/vkcube plus a game trace. Log temps, clocks and RAPL watts. Each test runs 3× and reports the median.
- **Phase 4 kernel:** `gentoo-power` = current CachyOS/BORE base, reproducible config in the repo, AutoFDO profile from real workloads, installkernel + dracut + GRUB entry. 7.3.0-tux and 6.18.57 stay as fallbacks.
- **Phase 5 toolchain:** GCC pgo+lto, ccache, mold (per-package where safe), tmpfs with automatic disk fallback for heavy packages, optional PGO Clang in /opt.
- **Phase 6 hardware:** BIOS checklist (EXPO, PBO, CO) for the user to do by hand. it87 fan probe. OverDrive. Idle and IO tuning.
- **Phase 7 app:** Gentoo Power Control: a root daemon (OpenRC service), a Unix socket with SO_PEERCRED and a group check, a GTK4 frontend, profiles 50–100%, capability detection, workload auto-switching.
- **Phase 8–10:** after-numbers, stability (stress-ng + memtest), report, public GitHub repo with no serials, IPs or hostnames.

## Ethernet (added 2026-11-10)

| Item | Found |
|---|---|
| NIC | Realtek RTL8125 2.5GbE (10ec:8125 rev 05), in-kernel r8169 driver, firmware files present |
| PCIe | 5 GT/s x1 (enough for 2.5G) |
| Link | **1000 Mb/s full**: the router/switch port is most likely 1G, so 2.5G needs a 2.5G port on the other end |
| Queues | 1 RX + 1 TX (no RSS on this chip/driver), so RPS/XPS and IRQ affinity are the real levers |
| qdisc / CC | fq_codel / cubic; tcp_bbr, sch_fq and sch_cake are built as modules |
| Buffers | rmem_max/wmem_max 4 MiB, netdev_max_backlog 1000, budget 300, busy_poll 0 |
| Config | OpenRC netifrc, `config_enp10s0="dhcp"` (dhcpcd); tux-netbond present (combining off) |
| Errors | 0 rx/tx errors, 0 drops so far |

Plan: measure first (iperf3 to a LAN peer, ping/fping jitter under load, i.e. a bufferbloat test, and the WAN separately). Then try BBR+fq vs cubic+fq_codel vs cake at the WAN rate, RPS onto CCD0 cores, IRQ pinning, coalescing and ring sizes, and EEE off. No jumbo frames unless every hop supports them. DHCP/netifrc stays untouched. Profiles: balanced / low-latency / throughput, tied to the 50–100% slider. The ISP or router limit gets reported as such, not as a gain.
