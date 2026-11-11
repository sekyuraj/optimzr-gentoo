# Changelog

## 0.1.0 (2026-11-11)

- Benchmark suite (`bench/run.sh`): CPU, memory, compile, compression,
  storage, network, GPU, plus settings sweeps (`levels`, `idlegov`, `gpucap`,
  `eth`). Median of 3, with package power, Tctl and clocks per test.
- Baseline of the stock system (`docs/baseline.md`).
- gentoo-power kernel: 7.3.3 + CachyOS patches, clang ThinLTO, two-stage
  AutoFDO build (`kernel/`).
- Gentoo Power Control: `optimzrd` (root daemon, OpenRC), `optimzr` (CLI),
  `optimzr-gui` (GTK4). Power levels 50–100%, network profiles, auto
  switching for games and builds, live sensors, full restore on `stock` and on
  service stop.
- `optimzr-root`: optional sudo helper with allow-lists for setup and
  benchmarking.
- Portage: GCC with PGO + LTO.
