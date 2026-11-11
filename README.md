# optimzr-gentoo

Performance work on one Gentoo desktop, written up so it can be repeated and
checked: a tuned kernel, a faster toolchain, a power-control app, and
benchmarks for every change. Settings that didn't measure better were
dropped.

The machine: Ryzen 9 9950X, Radeon RX 9070-series, 32 GB DDR5, PCIe 4.0
NVMe, RTL8125 Ethernet, OpenRC.

| part | what | doc |
|---|---|---|
| `bench/` | benchmark runner and the stock baseline | [baseline](docs/baseline.md) |
| `kernel/` | **gentoo-power**: 7.3.3 + CachyOS patches, clang ThinLTO, AutoFDO | [kernel](docs/kernel.md) |
| `app/` | **Gentoo Power Control**: 50–100% slider, network profiles, sensors | [power control](docs/power-control.md), [profiles](docs/profiles.md) |
| `portage/` | Portage and toolchain settings | [toolchain](docs/toolchain.md) |
| | Ethernet tuning | [ethernet](docs/ethernet.md) |
| | BIOS settings Linux can't change (EXPO, PBO) | [bios](docs/bios.md) |
| `root-helper/` | optional sudo helper used by the scripts | [security](SECURITY.md) |

## Results

See [docs/results.md](docs/results.md).

## Quick start

```sh
git clone https://github.com/sekyuraj/optimzr-gentoo
cd optimzr-gentoo
./install.sh                 # Gentoo Power Control (daemon, CLI, GTK4 window)
bench/run.sh before cpu mem  # numbers for your own machine
```

The kernel and Portage parts are machine-specific; read their docs first.

## Safety

- Nothing here disables thermal protection, changes voltages, writes MSRs or
  turns CPU mitigations off.
- Every runtime change is recorded and undone by `optimzr stock` or by
  stopping the service.
- New kernels install next to the old ones and never become the GRUB default
  on their own.
- Recovery: [docs/recovery.md](docs/recovery.md).

## License

MIT, see [LICENSE](LICENSE). Kernel configuration files under
`kernel/config/` are configuration for the GPL-2.0 Linux kernel.
