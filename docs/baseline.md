# Baseline (2026-11-10)

Stock state before any optimzr change: kernel 7.3.0-tux, amd-pstate-epp
`powersave` + EPP `balance_performance`, RAM 4800 MT/s (EXPO off), PBO off
(200 W PPT), GPU at defaults. Public services stopped during the runs. Each
number is the median of 3 runs. Watts = CPU package (RAPL) averaged over the test.

Raw table: `bench/results/baseline/summary.tsv`. Re-run: `bench/run.sh <label>`.

## CPU
| Test | Result | Pkg W | Tctl max |
|---|---|---|---|
| CoreMark 32 threads | 1149050 it/s | 189.8 | 78.8 °C |
| CoreMark 1 thread | 59672 it/s | 47.9 | |
| 7-Zip 32 threads | 168194 MIPS | 184.0 | |
| 7-Zip 1 thread | 8985 MIPS | | |
| stress-ng matrix | 115321 ops/s | 199.4 | |

All-core load stops at 199–200 W: the stock PPT. Tctl peaks around 78–84 °C.

## Memory (DDR5-4800)
| Test | Result |
|---|---|
| Latency in L1 / L2 / L3 | 0.91 / 3.20 / 8.90 ns |
| Latency 1 GiB block (DRAM) | 90.93 ns |
| Read bandwidth, 32 threads | 70.6 GB/s |
| Copy bandwidth | 54.9 GB/s |
| Single-thread read | 53.4 GB/s |

## Compiling (linux-7.3.3 defconfig, -j32)
| Toolchain | Time | Pkg W |
|---|---|---|
| GCC 16.2 | 70.03 s | 162.8 |
| Clang 22.1 | 74.51 s | 163.7 |

## Compression (1.6 GB kernel tree tarball)
| Test | Result |
|---|---|
| zstd -3 / -19, 32 threads | 5281.7 / 62.2 MB/s |
| zstd decompress | 2673.5 MB/s |
| xz -6, 32 threads | 91.8 MB/s |

## Storage (SIX X7400, DRAM-less, ext4, O_DIRECT)
| Test | Result |
|---|---|
| Sequential read / write 1M | 4684 / 2694 MB/s |
| 4K random read QD1 | 16660 IOPS |
| 4K random read QD256 | 573563 IOPS |
| 4K random write QD128 | 299445 IOPS |
| Sequential read of randomly-written data | 435 MB/s |

The last line is roughly 10× below a fresh sequential read. Typical for a DRAM-less
controller whose mapping table lives in a small host memory buffer.

## Network (no LAN peer: router, WAN, loopback)
| Test | Result |
|---|---|
| Router ping avg / jitter / loss | 0.931 / 0.165 ms / 0 % |
| WAN (1.1.1.1) idle avg / jitter | 11.004 / 0.355 ms |
| WAN while downloading avg / jitter | 11.790 / 0.864 ms |
| Single-stream download (kernel.org) | 81.7 Mbit/s |
| Loopback TCP stream | 144571 Mbit/s |
| Loopback TCP / UDP request-response | 300368 / 341034 per s |

The NIC links at 1 Gb/s because the router port only offers 1000BASE-T. The
download speed is set by the ISP/CDN path, not by this PC.

## GPU (RX 9070-series, vkpeak, Vulkan/RADV)
| Test | Result |
|---|---|
| FP32 scalar / vec4 | 33094.90 / 22965.42 GFLOPS |
| FP16 vec4 / matrix | 52117.31 / 197697.87 GFLOPS |
| INT8 matrix | 380305.93 GIOPS |
| Host↔device copy | 24.48 / 24.43 GB/s (PCIe 4.0 x16) |
| VRAM copy | 292.24 GB/s |
| Board power while busy | 289.9 W (cap 317 W, max allowed 340 W) |
| Junction temperature max | 92.0 °C |
