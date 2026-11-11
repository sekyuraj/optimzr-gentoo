# Ethernet

Realtek RTL8125 (2.5 GbE, `r8169` driver), PCIe 2.0 x1 link to the chipset.
It links at **1000 Mb/s** because the router port only offers 1000BASE-T, and
the internet connection is about 80 Mbit/s down / 32 up. So nothing on this
PC can make the internet faster, and this page doesn't claim it does. What it
can change is latency, and how this PC's own traffic behaves.

## What the hardware offers

| | |
|---|---|
| queues | 1 RX, 1 TX: RSS and XPS have nothing to spread |
| rings | 256/256, already the maximum |
| interrupt coalescing (`ethtool -C`) | not supported by the driver |
| driver batching | `gro_flush_timeout` 20 µs + `napi_defer_hard_irqs` 1 (r8169 default) |
| EEE | on and active (link sleeps between packets) |
| errors, drops, `rx_missed` | all 0 after days of uptime: nothing to fix with rings or backlog |
| MTU | 1500. Jumbo frames would need every device on the LAN and the router to agree, and the internet is 1500 anyway: not touched |

## Measured

`bench/run.sh eth eth`, median of 3. "Loaded" means pinging 1.1.1.1 while
a download (kernel.org) or an upload (Cloudflare's speed test endpoint)
fills the line.

### Latency to the router and to 1.1.1.1

| setting | router avg | router p99 | router sd | WAN avg | WAN p99 |
|---|---|---|---|---|---|
| stock (EEE on, 20 µs batching) | 0.905 ms | 1.870 ms | 0.640 | 10.92 ms | 14.1 ms |
| EEE off, no batching | **0.834 ms** | **1.620 ms** | 0.504 | 10.84 ms | **12.2 ms** |

−8% average and −13% at p99 to the router. On the WAN it's in the noise
except the p99. This is the **Low latency** profile.

### Congestion control and queueing

| | down | ping while downloading | up | ping while uploading |
|---|---|---|---|---|
| cubic + fq_codel (stock) | 79.9 Mbit/s | 11.7 ms | 32.1 Mbit/s | **74.8 ms** (p99 86.9) |
| **BBR + fq** | 79.5 Mbit/s | 11.7 ms | 31.7 Mbit/s | **25.8 ms** (p99 29.4) |
| cubic + cake (no shaping) | 77.0 Mbit/s | 11.6 ms | 32.2 Mbit/s | 74.4 ms |

With cubic, an upload fills the modem's buffer and everything else waits
behind it: game pings, voice, SSH, and the services this PC hosts. BBR
paces its sending to the measured bottleneck instead of filling buffers:
same upload speed, **65% less latency** while uploading. It only acts on
traffic this PC sends (uploads and everything the servers on it send out),
so downloads are unchanged, as expected.

cake without a bandwidth setting behaves like fq_codel here. The buffer that
fills is in the modem, not in this PC; cake could only help by shaping below
the upload speed, which costs upload speed. BBR gets the same effect without
that.

## Profiles

| profile | settings | when |
|---|---|---|
| Balanced | BBR + fq | levels 50–90 (default) |
| Low latency | BBR + fq, EEE off, no interrupt batching | level 100, or pick it |
| Throughput | BBR + fq | identical to Balanced on this machine: with one queue, a 1 Gb link and an 80 Mbit line nothing else measured faster |

Ideas that were dropped because they can't win here: RPS/XPS and IRQ
affinity (one queue, and the CPU is never the limit at 1 Gb/s), bigger TCP
buffers (bandwidth × delay is 80 Mbit/s × 11 ms ≈ 110 KB, far below the
defaults), ring sizes (already at the maximum, zero drops).

## Safety

- DHCP, DNS and `/etc/conf.d/net` are never touched. Everything is runtime
  only, recorded first, and undone on `optimzr stock` or when optimzrd stops.
- Turning EEE on or off makes the link renegotiate, which drops it for about
  3 seconds. That only happens when switching to or from Low latency.
