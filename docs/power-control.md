# Gentoo Power Control

A power slider (50–100%), a network profile, an optional "go to 100% for games
and builds" switch, and live readings for CPU, GPU, memory, fans, temperatures
and network.

```
optimzr-gui   (GTK4, runs as you)  ─┐
optimzr       (CLI, runs as you)   ─┼─ /run/optimzr/optimzrd.sock ── optimzrd (root, OpenRC)
                                    ┘   SO_PEERCRED + group check       sysfs, procfs, ethtool, tc
```

## Install

```sh
./install.sh          # builds, installs, enables and starts optimzrd
./uninstall.sh        # stops it (which restores every setting) and removes it
```

Needs `gtk4` for the window; the daemon and CLI need only libc. With plain
sudo, `install.sh` creates an `optimzr` group and adds you to it.

## Using it

```sh
optimzr               # current level, network profile, auto, what's running
optimzr 90            # set the level
optimzr net lowlatency
optimzr auto on
optimzr watch         # live sensors in the terminal
optimzr caps          # what this machine supports
optimzr stock         # undo everything, stop managing
```

The level you pick is saved in `/var/lib/optimzr/optimzrd.state` and applied
again at boot. `stock` is saved too: after it, optimzrd changes nothing until
you pick a level again.

## What the levels do

See [profiles.md](profiles.md) for the table and the measurements behind each
row. In short: below 80 the CPU's top clocks are capped and the GPU power
limit goes down; 80 is how the machine ships; 90 and 100 make the CPU clock up
sooner and tell the GPU driver it's running a game.

## Network profiles

"Follow power level" uses Balanced up to 90% and Low latency at 100%. You can
pin one instead. See [ethernet.md](ethernet.md).

## Auto

Off by default. When on, optimzrd looks at process names every two seconds:

- **build**: 4 or more compiler/linker processes (`cc1`, `cc1plus`, `clang`,
  `rustc`, `ld.lld`, `mold`, ...)
- **game**: wine/proton (`wine64-preload`, `wineserver`), `gamescope`, or an
  emulator (`dolphin-emu`, `rpcs3`, `pcsx2-qt`); a running `*.exe` counts
  when the GPU is over 50% busy

It raises the level to 100% straight away and drops back to your level 30
seconds after the last one ends. It never goes below what you picked, and
turning it off puts your level back.

## Monitoring

Read once a second by optimzrd (it's root, so it can read RAPL):

| | source |
|---|---|
| CPU clock, usage | `cpufreq/scaling_cur_freq`, `/proc/stat` |
| CPU package power | RAPL `energy_uj` (AMD exposes it as intel-rapl) |
| CPU temperatures | k10temp Tctl, Tccd1, Tccd2 |
| GPU clock, usage, power, VRAM, temperatures, fan | amdgpu sysfs and hwmon |
| board fans | it87 (IT8689E on this board), read-only |
| board, SSD temperature | gigabyte_wmi, nvme hwmon |
| network | `/sys/class/net/<if>/statistics` |

Fan *control* stays in the BIOS (Smart Fan). The kernel warns that ACPI also
uses the IT8689E's I/O ports; reading is harmless, but writing PWM values from
Linux while the firmware does the same is how fans end up stuck.

## Logs and files

| | |
|---|---|
| `/var/log/optimzrd.log` | every change, who asked for it, workload switches |
| `/var/lib/optimzr/optimzrd.state` | level, network profile, auto |
| `/var/lib/optimzr/optimzrd.stock` | original value of every knob it has touched |
| `/etc/conf.d/optimzrd` | which group may connect |

## Testing without root

```sh
app/build/optimzrd -n -s /tmp/o.sock &          # dry run: logs writes, changes nothing
OPTIMZR_SOCK=/tmp/o.sock app/build/optimzr 60
OPTIMZR_SOCK=/tmp/o.sock app/build/optimzr-gui
```
