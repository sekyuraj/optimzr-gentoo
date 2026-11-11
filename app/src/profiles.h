/* Power levels for the slider. Every value here was measured; see
 * docs/profiles.md for the numbers behind each row. */
#ifndef PROFILES_H
#define PROFILES_H

struct level {
    int pct;
    const char *governor;  /* cpufreq governor */
    const char *epp;       /* amd-pstate energy_performance_preference */
    int boost;             /* cpufreq/boost */
    int cpu_max_mhz;       /* 0 = no cap */
    const char *idle_gov;  /* cpuidle governor, NULL = leave */
    int gpu_cap_pct;       /* of the default power cap, clamped to min..max */
    const char *gpu_mode;  /* pp_power_profile_mode name */
    const char *net;       /* network profile used when net = auto */
    const char *summary;   /* one line for the UI */
};

/* filled in from measurements, see docs/profiles.md */
static const struct level levels[] = {
    {50, "powersave", "power", 0, 0, "teo", 70, "BOOTUP_DEFAULT", "balanced",
     "No boost (4.3 GHz max), GPU limited to 221 W. Quiet and cool."},
    {60, "powersave", "balance_power", 1, 4500, "teo", 80, "BOOTUP_DEFAULT", "balanced",
     "Boost up to 4.5 GHz, GPU at 80% power."},
    {70, "powersave", "balance_power", 1, 4800, "teo", 90, "BOOTUP_DEFAULT", "balanced",
     "Boost up to 4.8 GHz, GPU at 90% power."},
    {80, "powersave", "balance_performance", 1, 0, "teo", 100, "BOOTUP_DEFAULT", "balanced",
     "As shipped: full boost, default GPU power limit (plus the faster teo idle governor)."},
    {90, "powersave", "performance", 1, 0, "teo", 100, "BOOTUP_DEFAULT", "balanced",
     "CPU clocks up sooner (EPP performance). GPU stock."},
    {100, "performance", "performance", 1, 0, "teo", 110, "BOOTUP_DEFAULT", "lowlatency",
     "Performance governor, GPU power limit raised to 340 W (the driver maximum)."},
};
#define NLEVELS (int)(sizeof levels / sizeof *levels)
#define STOCK_LEVEL 80

/* network profiles */
struct netprof {
    const char *name;
    const char *cc;       /* tcp_congestion_control */
    const char *qdisc;    /* root qdisc on the wired NIC */
    int eee;              /* 1 on, 0 off, -1 leave */
    int rps;              /* steer receive work to the CCD0 cores */
    int gro_flush_us;     /* gro_flush_timeout in us, -1 leave */
    const char *summary;
};

static const struct netprof netprofs[] = {
    {"balanced", "bbr", "fq", -1, 0, -1,
     "BBR + fq: ping while uploading 26 ms instead of 75 ms, same speed."},
    {"lowlatency", "bbr", "fq", 0, 0, 0,
     "BBR + fq, Energy Efficient Ethernet off, no interrupt batching."},
    {"throughput", "bbr", "fq", -1, 0, -1,
     "Same as balanced: nothing else measured faster on a 1 Gb link."},
};
#define NNETPROFS (int)(sizeof netprofs / sizeof *netprofs)

#endif
