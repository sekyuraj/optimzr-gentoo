# Power levels: what they set and what they measure

Measured with `bench/run.sh levels`, `levels70`, `idlegov` and `gpucap`, median
of 3 (GPU: 1 run, vkpeak varies under 0.3%). Package power is RAPL. Public
services were stopped during the runs. Stock RAM (4800 MT/s), PBO off.

## CPU

| level | governor / EPP | clock limit | idle W | CoreMark 1T | CoreMark 32T | W under 32T | Tctl max | TCP_RR/s |
|---|---|---|---|---|---|---|---|---|
| 50 | powersave / power | boost off (4.3 GHz) | 24.2 | 45 420 (−25%) | 1 001 983 (−15%) | 107 | 49 °C | 244 020 (−18%) |
| 60 | powersave / balance_power | 4.5 GHz | 25.9 | 47 695 (−21%) | 1 025 203 (−13%) | 120 | 54 °C | 258 686 (−13%) |
| 70 | powersave / balance_power | 4.8 GHz | 28.6* | 50 565 (−16%) | 1 081 015 (−8.5%) | 152 | 63 °C | 271 705 (−8%) |
| **80** | powersave / balance_performance | none (5.76 GHz) | 25.0 | 60 403 | 1 181 327 | 190 | 78 °C | 296 639 |
| 90 | powersave / performance | none | 25.8 | 60 333 (0%) | 1 152 059 (−2%, noise) | 191 | 77 °C | 304 079 (+2.5%) |
| 100 | performance | none | 27.6 | 60 430 (0%) | 1 184 467 (0%) | 190 | 77 °C | 301 836 (+1.8%) |

80 is how the machine ships. The 9950X holds its 200 W package limit under
all-core load at 80, 90 and 100, so the higher levels can't add
throughput; what 90 and 100 buy is quicker clock ramp-up, which shows in
short request/response work (+2–2.5% TCP_RR) at 1–3 W more at idle. Real
multi-core gains need PBO in the BIOS ([bios.md](bios.md)).

A 5.2 GHz cap was tried first for 70 and did nothing under all-core load
(the CPU already runs ~4.8 GHz there): 1 150 966 at 189 W, same as stock.
The 70 row uses a 4.8 GHz cap instead. 4.6 GHz was also measured (1 033 703 at 127 W) and lands on top of level 60, so it isn't a useful step.

\* measured on a different day than the other rows; idle power that day was about 3 W higher across the board (stock settings measured 29.5 W idle in the same run).

## Idle governor

The kernel's default cpuidle governor here is `menu`. `teo` (timer events
oriented) picks idle states from how long the CPU actually slept before,
which suits Zen 5's deep C-state (350 µs exit latency) better:

| governor | idle W | CoreMark 1T | TCP_RR/s (loopback) |
|---|---|---|---|
| menu (default) | 29.4 | 59 959 | 296 275 |
| **teo** | 29.5 | 60 080 | **384 147 (+30%)** |

Same idle power, same single-thread speed, 30% more round trips per second.
An earlier run at a 4.3 GHz clock limit showed the same direction (+14%).
Every level uses `teo`; it's a runtime switch
(`/sys/devices/system/cpu/cpuidle/current_governor`), no reboot needed.

## GPU (RX 9070-series, vkpeak FP32 vec4)

| power limit | used for | GFLOPS | W while busy | hotspot max |
|---|---|---|---|---|
| 222 W (70%) | level 50 | 21 539 (−6.2%) | 215 | 78 °C |
| 254 W (80%) | level 60 | 21 962 (−4.4%) | 243 | 89 °C |
| 285 W (90%) | level 70 | 22 579 (−1.7%) | 270 | 88 °C |
| **317 W (stock)** | 80, 90 | 22 962 | 292 | 93 °C |
| 340 W (driver max) | level 100 | 23 204 (+1.1%) | 300 | 89 °C |

The power profile mode `3D_FULL_SCREEN` measured 23 024 GFLOPS at 317 W
(+0.3%, inside the noise), so every level leaves it at the default.
Clock/voltage OverDrive is disabled by the default `amdgpu.ppfeaturemask`
and isn't used.
