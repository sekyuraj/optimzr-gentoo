# Recovery and troubleshooting

## Undo everything Gentoo Power Control changed

```sh
optimzr stock                 # restore and stop managing
sudo rc-service optimzrd stop # same, and the daemon exits
./uninstall.sh                # stop and remove
```

The original values are in `/var/lib/optimzr/optimzrd.stock` (one
`path<TAB>value` per line) if you ever need to put something back by hand.
Runtime settings also go back to the kernel defaults on reboot if optimzrd
isn't running.

## Settings changed by the benchmark helper

```sh
sudo optimzr-root revert-runtime   # sysfs/sysctl values written through the helper
sudo optimzr-root backups          # files it replaced, by timestamp
sudo optimzr-root restore <stamp> /etc/portage/make.conf
```

## Kernel

| symptom | fix |
|---|---|
| gentoo-power hangs or panics at boot | reboot, GRUB → Advanced options → the previous kernel |
| it boots but something misbehaves | boot the previous kernel, `sudo optimzr-root kernel-remove <release>` |
| no kernel boots | GRUB → Advanced options → the distribution kernel (`*-gentoo-dist-bin`) |
| GRUB menu is missing | hold Shift (BIOS) or press Esc (UEFI) during boot |
| `grub.cfg` broken | live USB, mount root and `/boot/efi`, chroot, `grub-mkconfig -o /boot/grub/grub.cfg` |

## Toolchain

If the PGO/LTO GCC misbehaves (internal compiler errors that weren't there
before):

```sh
sudo rm /etc/portage/package.use/optimzr-toolchain
sudo emerge -1 sys-devel/gcc
```

The previous GCC stays usable meanwhile; `gcc-config -l` lists the installed
versions.

## BIOS

See [bios.md](bios.md). Clear CMOS to get back to defaults.

## Troubleshooting

| | |
|---|---|
| `optimzr: connect ... Permission denied` | you're not in the group from `/etc/conf.d/optimzrd`; log out and in after being added |
| the window says optimzrd is not running | `sudo rc-service optimzrd start`, then check `/var/log/optimzrd.log` |
| no fan speeds | the board needs `it87` (Gigabyte/ITE) or `nct6775` (ASUS/MSI/ASRock); `install.sh` sets up it87 on Gigabyte boards |
| CPU W shows 0 | the kernel has no RAPL for this CPU (`/sys/class/powercap/intel-rapl:0`) |
| a level doesn't change the GPU | `optimzr caps` shows what the driver exposes; amdgpu needs `power1_cap` and `pp_power_profile_mode` |
