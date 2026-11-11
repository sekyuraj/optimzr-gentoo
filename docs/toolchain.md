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

GCC_RESULT

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
| clang/LLVM PGO or LTO | Gentoo's LLVM ebuilds have no `pgo`/`lto` flag; a hand-rolled LTO link of LLVM needs more than 32 GB |
| mold as the default linker | Portage builds aren't link-bound here, and some packages can't use it |
| tmpfs `PORTAGE_TMPDIR` | chromium/llvm builds need 20+ GB of temp space, too much for 32 GB RAM; the NVMe isn't the bottleneck while compiling |

The kernel itself is built with clang ThinLTO and AutoFDO, see
[kernel.md](kernel.md).
