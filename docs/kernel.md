# The gentoo-power kernel

7.3.3 with the CachyOS patch set (BORE scheduler, ADIOS I/O scheduler and the
rest of their base patches), built with clang 22 ThinLTO, `-O3`,
`-march=native`, and an AutoFDO profile recorded on this machine.

It is built in two stages, because AutoFDO needs a profile taken on a kernel
built from the same source:

| stage | release | what it is |
|---|---|---|
| 1 | `7.3.3-gentoo-power-train` | same config as 7.3.0-tux, AutoFDO debug info, no profile |
| 2 | `7.3.3-gentoo-power` | rebuilt with the profile recorded on stage 1 |

## Build it

```sh
cd kernel
./prepare.sh                         # copies the CachyOS 7.3 tree, applies patch-7.3.3 (sha256 checked)
./build.sh --install                 # stage 1, ~6 min on a 9950X
# reboot, pick "7.3.3-gentoo-power-train" under "Advanced options" in GRUB
./profile.sh                         # ~4 min of mixed load under perf, writes profile/kernel.afdo
./build.sh --install profile/kernel.afdo   # stage 2
# reboot into 7.3.3-gentoo-power, then benchmark it
```

`prepare.sh` takes the base tree as an argument (default `/usr/src/linux-7.3.0-tux`).
Any CachyOS-patched 7.3 tree works; a vanilla 7.3 tree also works if you drop the
Cachy-only options (`SCHED_BORE`, `MQ_IOSCHED_ADIOS`, `CACHY`) from the config.

### What changed against 7.3.0-tux

`config/gentoo-power.fragment` is the whole diff to the old config:

- `LOCALVERSION`: `-gentoo-power-train` / `-gentoo-power`, so both stages and
  the old kernel can be installed side by side.
- `AUTOFDO_CLANG=y`: lets clang use the perf profile for inlining, block
  layout and hot/cold splitting.
- 7.3.0 → 7.3.3 stable fixes.

Things that were looked at and left alone, with the reason:

| option | why it stays |
|---|---|
| `CPU_MITIGATIONS` | this PC runs public services; the cost is a few % on syscall-heavy loads only |
| `INIT_ON_ALLOC_DEFAULT_ON` | hardening, can be turned off at boot (`init_on_alloc=0`) if you want to measure it |
| `HZ=1000`, `PREEMPT_DYNAMIC`, `NO_HZ_FULL` | already the low-latency choices |
| idle governor | `menu` vs `teo` is a runtime switch; measured in `docs/profiles.md` |
| `SCHEDSTATS`, `SLUB_DEBUG`, `KFENCE`, `PAGE_POISONING` | built in but off at runtime, no measurable cost |

### The AutoFDO profile

`profile.sh` checks that the running kernel's build id matches the
`vmlinux` it saved, then runs `perf record -e ex_ret_brn_tkn:k -b` (Zen 4/5
LBRv2 branch records, kernel only) for the whole system while it compiles
a kernel, does random and sequential disk I/O, TCP over loopback and file
metadata churn. Your normal services keep running in the background, which
is what you want: the profile should look like your real day.
`llvm-profgen --kernel` turns that into `profile/kernel.afdo`.

`perf_event_paranoid` and `kptr_restrict` are lowered for the recording and
put back afterwards, also when it's interrupted.

## Install and GRUB

`build.sh --install` calls `optimzr-root kernel-install`, which runs
`make modules_install install`. Gentoo's installkernel then builds the
initramfs with dracut and regenerates `grub.cfg`.

The helper refuses to install a release that isn't called `*gentoo-power*` or
is the running kernel, so it can't overwrite the kernel you booted from.

GRUB's default entry is pinned to 7.3.0-tux in `/etc/default/grub`
(`GRUB_DEFAULT="gnulinux-advanced-<uuid>>gnulinux-7.3.0-tux-advanced-<uuid>"`),
so a new kernel never becomes the default by itself. You choose gentoo-power by
hand under "Advanced options" until it has booted and passed the benchmarks.
Then the pin is moved to it.

## Rollback

| problem | do this |
|---|---|
| gentoo-power doesn't boot | reboot, GRUB → Advanced options → 7.3.0-tux (still the default) |
| boots, but something is off | same; then `sudo optimzr-root kernel-remove 7.3.3-gentoo-power` |
| nothing boots | GRUB → Advanced options → 6.18.57-gentoo-dist-bin (Gentoo's binary kernel, untouched) |
| GRUB itself is broken | boot a Gentoo live USB, mount root, `chroot`, `grub-mkconfig -o /boot/grub/grub.cfg` |

`/etc/default/grub` is backed up by the helper before every change
(`optimzr-root backups`, `optimzr-root restore <stamp> /etc/default/grub`).
