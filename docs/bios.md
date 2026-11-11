# BIOS settings (Gigabyte B650 GAMING X AX V2, BIOS F38)

Linux can't change these, and they're the biggest wins on this machine. Do one
step at a time, boot, and run `bench/run.sh <label>` (or at least the stress
test below) before the next step. Menu names are from Gigabyte's AM5 BIOS and
can differ slightly between versions. Press F2 to switch from Easy to Advanced Mode.

Recovery for every step: if it won't POST, wait. Memory training can take
1–3 minutes with a black screen on the first boot after a RAM change. If it
still fails, the board falls back after three failed boots. If that doesn't
happen either, clear CMOS (the CLR_CMOS jumper/pins near the battery, see the
manual) and you're back to defaults.

## Measured now

| | Now | Why it matters |
|---|---|---|
| RAM | 4800 MT/s, 1.10 V (JEDEC fallback) | kit is rated EXPO 6000 CL36 1.35 V |
| CPU package power | holds at ~200 W under all-core load | that is the stock 9950X PPT; PBO is off |
| Tctl at 200 W | ~78 °C | 17 °C headroom to the 95 °C limit |
| Resizable BAR | on (16 GB BAR0) | nothing to do |

## Step 1: EXPO (memory)

Tweaker → **Extreme Memory Profile (X.M.P./EXPO)** → **EXPO I** (DDR5-6000 36-36-36-76).

- Leave FCLK on Auto. At 6000 the board runs UCLK = MCLK (1:1), which is what you want.
- Optional, after EXPO is stable: Tweaker → **High Bandwidth Support** and **Low Latency Support**
  → Enabled. These are Gigabyte's tighter sub-timings. Test again after.
- Settings → AMD CBS / Memory → **Memory Context Restore** → Enabled, if listed. It skips retraining on every boot.

Expect: about 25% more memory bandwidth and lower latency. This helps games,
compiling big C++, compression and anything else that misses the 32 MB L3.

Stability check, Linux: `memtester 24G 1` (as root) or
`stress-ng --vm 16 --vm-bytes 85% -t 30m --verify`. Any error = back off.

## Step 2: PBO (CPU power limits)

Tweaker → Advanced CPU Settings → **Precision Boost Overdrive** → **Advanced**:

- **PBO Limits: Motherboard.** This lifts PPT/TDC/EDC to the board's VRM limits.
  Temperature is still capped at 95 °C by the CPU itself; that protection can't be turned off here.
- **Max CPU Boost Clock Override: +100 MHz.** Raises the single-core ceiling 5.7 → 5.8 GHz.
- **Curve Optimizer: All Cores, Negative, 10** to start. That's a voltage offset per boost
  point: less heat for the same clock, so it boosts higher. Go to 15–20 only if
  step 3 passes; per-core values are possible later.

Expect: multi-core +5–12% (it will now use 230–260 W), single-core +1–3%.

## Step 3: stability before trusting it

Curve Optimizer instability shows up as rare crashes when the machine is
*idle or lightly loaded*, not only under full load. So test both:

```
stress-ng --cpu 32 --cpu-method all -t 20m --verify     # heat
bench/run.sh pbo cpu compile                            # real work + numbers
```

and then use the PC normally for a day. If you see a random reboot, a WHEA/MCE line
in `dmesg` (`mce: [Hardware Error]`), or a compile that fails with an
internal compiler error, lower the Curve Optimizer magnitude by 5.

## Not recommended

- Manual all-core overclock or fixed vcore: loses single-core boost and wastes power.
- Turning off C-states / Global C-state Control: Zen 5 boosts *higher* when idle cores can sleep.
- Running the memory above the EXPO profile (6400+ needs 1:2 UCLK on many kits): latency usually gets worse.
