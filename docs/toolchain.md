# Toolchain and Portage

## What was there

`-march=native -O2 -pipe`, `MAKEOPTS="-j32 -l32"`, 4 parallel emerge jobs,
`-j14` for the memory-hungry C++ packages (`env/heavy.conf`) so 32 GB is
enough. GCC 16.2.1 and clang/LLVM 22 were built without PGO or LTO.

## Changes

### GCC with PGO + LTO

```
# /etc/portage/package.use/optimzr-toolchain
sys-devel/gcc pgo lto
```

`pgo` builds GCC three times (stage 1, an instrumented stage, then the final
compiler using the profile from building itself); `lto` links it with
link-time optimisation. The compiler gets faster, the code it produces is the
same. Measured with the kernel defconfig build from `bench/run.sh compile`:

| | kernel defconfig, gcc | same build, clang (control) |
|---|---|---|
| before (GCC 16.2.1 plain) | 70.03 s (70.00–70.18) | 74.51 s |
| **after (pgo + lto)** | **64.20 s (64.19–64.40), −8.3%** | 77.12 s (+3.5%) |

The GCC build got 8.3% faster with three runs inside 0.2 s of each other.
The clang build, which shouldn't have changed, got 3.5% slower. Ruled out:
the `teo` idle governor (79.5 s under both `menu` and `teo`), optimzrd (79.6 s
with it stopped), the it87 sensor driver (79.9 s unloaded). The one thing
clang shares with the new GCC is `libstdc++.so.6`, which was rebuilt with it;
that is the likely cause, not yet proven. Either way GCC's own gain is at
least 8.3%, more if the same slowdown applied to it.

The rebuild takes about 1 h 45 min on the 9950X.

### LLVM and clang built with clang, -O3 and ThinLTO

Gentoo has no flag for it, so it's a `package.env` entry
(`portage/env/optimzr-llvm-lto.conf`, `portage/package.env/optimzr-llvm`):
clang as the compiler, `-O3 -flto=thin -march=native`, lld as the linker,
16 jobs so the ThinLTO links fit in 32 GB. 20 minutes for llvm (64 + 32 bit)
and clang on the 9950X.

| kernel defconfig with clang (`LLVM=1`) | time |
|---|---|
| before | 74.38 s |
| **after** | **70.40 s (70.36–70.43), −5.4%** |

### ccache

```
# /etc/portage/make.conf
FEATURES="${FEATURES} ccache"
CCACHE_DIR="/var/cache/ccache"
```

Rebuilding a package that was built before (USE change, revdep rebuild,
re-emerge) reuses the cached objects. Measured with `app-arch/zstd`: 15–18 s
without, 6.0 s with a warm cache (2.5× faster). The first build showed no
measurable cost. Default cache size 5 GB.

### Not changed, and why

| idea | why not |
|---|---|
| `-O3` globally | no system-wide gain to measure, and it breaks a few packages; kept per-package (`env/optimized.conf`) |
| mold as the default linker | Portage builds aren't link-bound here, and some packages can't use it |
| tmpfs `PORTAGE_TMPDIR` | chromium/llvm builds need 20+ GB of temp space, too much for 32 GB RAM; the NVMe isn't the bottleneck while compiling |

The kernel itself is built with clang ThinLTO and AutoFDO, see
[kernel.md](kernel.md).
