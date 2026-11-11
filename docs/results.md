# Results

Everything below was measured on the machine described in the README with
`bench/run.sh`: median of 3 runs, public services stopped, same benchmark
binaries before and after. "Stock" is [baseline.md](baseline.md).

## What got better

| area | change | before | after | |
|---|---|---|---|---|
| CPU wake-up latency | `teo` idle governor (all levels) | 296 275 TCP_RR/s | 384 147 | **+30%** |
| upload bufferbloat | BBR + fq (all network profiles) | 74.8 ms ping while uploading | 25.8 ms | **−65%** |
| LAN latency | Low latency profile (EEE off, no IRQ batching) | 0.905 ms avg / 1.87 ms p99 | 0.834 / 1.62 | −8% / −13% |
| GPU compute | 340 W power limit (level 100) | 22 962 GFLOPS | 23 204 | +1.1% |
| package rebuilds | ccache | 15–18 s (zstd) | 6.0 s | 2.5× |
| compiling with GCC | GCC built with PGO + LTO | 70.03 s kernel defconfig | 64.20 s | **−8.3%** |
| kernel: network stack | gentoo-power (7.3.3, ThinLTO + AutoFDO) | 382 000 TCP_RR/s, 442 797 UDP_RR/s | 412 180, 488 476 | **+7.9%, +10.3%** |
| kernel: storage reads | gentoo-power | 18 539 IOPS QD1, 611 854 QD256 | 26 720, 844 645 | **+44%, +38%** |

## What the power levels trade

From [profiles.md](profiles.md): level 50 runs all-core work at 107 W
instead of 190 W for 15% less throughput; level 70 at 152 W for 8.5% less.
Levels 90 and 100 can't add multi-core throughput because the CPU sits at its
200 W package limit already; they add 2–2.5% on short request/response work.

## What didn't help (and was dropped)

| tried | result |
|---|---|
| GPU `3D_FULL_SCREEN` power profile | +0.3%, noise |
| cake qdisc without shaping | same as fq_codel (74.4 ms loaded) |
| 5.2 GHz cap for level 70 | no effect under all-core load |
| RPS/XPS, IRQ affinity, bigger rings/buffers | not measurable on a 1-queue NIC at 1 Gb/s with an 80 Mbit line |

## Still on the table

The two biggest wins on this machine are BIOS settings Linux can't change:
RAM runs at 4800 MT/s instead of its EXPO 6000 profile, and PBO is off, so
the CPU stops at 200 W with ~17 °C of thermal headroom left.
[bios.md](bios.md) has the steps; they're not done yet and not included above.
