/* optimzrd: root daemon for Gentoo Power Control.
 *
 * Owns every knob the app can change. Clients talk to it over a Unix socket
 * (/run/optimzr/optimzrd.sock) with a small line protocol, see proto.h.
 * Only root and members of the configured group may connect (SO_PEERCRED).
 * Clients choose from fixed profiles; they never send paths or values.
 *
 *   optimzrd [-f] [-g group] [-n] [-s socket]
 *     -f  stay in the foreground, log to stderr
 *     -n  dry run: change nothing, log what would be written (no root needed)
 *     -s  socket path (default /run/optimzr/optimzrd.sock)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "profiles.h"
#include "proto.h"

#define STATE_DIR "/var/lib/optimzr"
#define STATE_FILE STATE_DIR "/optimzrd.state"
#define STOCK_FILE STATE_DIR "/optimzrd.stock"
#define LOG_FILE "/var/log/optimzrd.log"
#define MAX_CLIENTS 16

static int foreground, dry;
static const char *sock_path = PROTO_SOCK;
static FILE *logf;
static volatile sig_atomic_t quit;

/* ------------------------------------------------------------- logging */

static void logmsg(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%F %T", localtime(&t));
    va_list ap;
    FILE *o = foreground ? stderr : logf;
    if (!o)
        return;
    fprintf(o, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(o, fmt, ap);
    va_end(ap);
    fputc('\n', o);
    fflush(o);
}

/* --------------------------------------------------------------- files */

static int read_str(const char *path, char *buf, size_t sz)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = 0;
    return (int)n;
}

static long read_long(const char *path)
{
    char b[64];
    if (read_str(path, b, sizeof b) <= 0)
        return -1;
    return strtol(b, NULL, 10);
}

static long long read_ll(const char *path)
{
    char b[64];
    if (read_str(path, b, sizeof b) <= 0)
        return -1;
    return strtoll(b, NULL, 10);
}

static int raw_write(const char *path, const char *val)
{
    if (dry) {
        logmsg("would write %s = %s", path, val);
        return 0;
    }
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    ssize_t n = write(fd, val, strlen(val));
    int e = n < 0 ? -errno : 0;
    close(fd);
    return e;
}

static int exists(const char *p) { return access(p, F_OK) == 0; }

/* ---------------------------------------------------------- stock values
 * The first time we touch a knob we record what it was, so "stock" and
 * stopping the service put the machine back exactly. One "key\tvalue" per
 * line. Keys are sysfs/procfs paths or "eee:<if>" / "qdisc:<if>". */

struct kv {
    char key[200];
    char val[200];
};
static struct kv stock[512];
static int nstock;

static void stock_load(void)
{
    FILE *f = fopen(STOCK_FILE, "re");
    char line[420];
    nstock = 0;
    while (f && nstock < 512 && fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        line[strcspn(line, "\n")] = 0;
        tab[1 + strcspn(tab + 1, "\n")] = 0;
        snprintf(stock[nstock].key, sizeof stock[0].key, "%s", line);
        snprintf(stock[nstock].val, sizeof stock[0].val, "%s", tab + 1);
        nstock++;
    }
    if (f)
        fclose(f);
}

static void stock_save(void)
{
    if (dry)
        return;
    char tmp[] = STOCK_FILE ".tmp";
    FILE *f = fopen(tmp, "we");
    if (!f)
        return;
    for (int i = 0; i < nstock; i++)
        fprintf(f, "%s\t%s\n", stock[i].key, stock[i].val);
    fclose(f);
    rename(tmp, STOCK_FILE);
}

static int stock_has(const char *key)
{
    for (int i = 0; i < nstock; i++)
        if (!strcmp(stock[i].key, key))
            return 1;
    return 0;
}

static void stock_add(const char *key, const char *val)
{
    if (stock_has(key) || nstock >= 512)
        return;
    snprintf(stock[nstock].key, sizeof stock[0].key, "%s", key);
    snprintf(stock[nstock].val, sizeof stock[0].val, "%s", val);
    nstock++;
    stock_save();
}

/* the value a knob shows in sysfs is not always what you write back */
static void current_value(const char *path, char *out, size_t sz)
{
    char b[4096];
    out[0] = 0;
    if (read_str(path, b, sizeof b) < 0)
        return;
    if (strstr(path, "pp_power_profile_mode")) {
        /* "  1 3D_FULL_SCREEN*:" -> "1" */
        char *star = strchr(b, '*');
        if (!star)
            return;
        char *p = star;
        while (p > b && p[-1] != '\n')
            p--;
        snprintf(out, sz, "%ld", strtol(p, NULL, 10));
        return;
    }
    if (strstr(path, "scaling_governor") || strstr(path, "current_governor") ||
        strstr(path, "energy_performance_preference")) {
        b[strcspn(b, " \n")] = 0;
    }
    snprintf(out, sz, "%s", b);
}

static void remember(const char *path)
{
    char cur[200];
    current_value(path, cur, sizeof cur);
    if (cur[0])
        stock_add(path, cur);
}

/* write a knob, remembering its stock value first */
static int set_knob(const char *path, const char *val)
{
    char cur[200];
    current_value(path, cur, sizeof cur);
    if (!cur[0] && !exists(path))
        return -ENOENT;
    stock_add(path, cur);
    if (!strcmp(cur, val))
        return 0;
    int e = raw_write(path, val);
    if (e)
        logmsg("write %s = %s failed: %s", path, val, strerror(-e));
    return e;
}

/* ---------------------------------------------------------------- exec */

static int run(char *const argv[], char *out, size_t outsz)
{
    if (dry && !out) {
        logmsg("would run %s %s %s %s", argv[0], argv[1] ? argv[1] : "", argv[1] && argv[2] ? argv[2] : "",
               argv[1] && argv[2] && argv[3] ? argv[3] : "");
        return 0;
    }
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC))
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (!pid) {
        static char *const env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL};
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(pfd[1], 1);
        dup2(null, 2);
        execve(argv[0], argv, env);
        _exit(127);
    }
    close(pfd[1]);
    size_t got = 0;
    char sink[512];
    for (;;) {
        char *dst = out && got < outsz - 1 ? out + got : sink;
        size_t room = out && got < outsz - 1 ? outsz - 1 - got : sizeof sink;
        ssize_t n = read(pfd[0], dst, room);
        if (n <= 0)
            break;
        if (dst != sink)
            got += (size_t)n;
    }
    if (out)
        out[got] = 0;
    close(pfd[0]);
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *find_bin(const char *name)
{
    static char buf[4][64];
    static int slot;
    const char *dirs[] = {"/usr/sbin", "/usr/bin", "/sbin", "/bin"};
    for (int i = 0; i < 4; i++) {
        char *b = buf[slot];
        snprintf(b, 64, "%s/%s", dirs[i], name);
        if (!access(b, X_OK)) {
            slot = (slot + 1) % 4;
            return b;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ hardware */

static struct {
    int ncpu;
    long cpu_max_khz, cpu_base_khz;
    int has_epp, has_boost, has_idle_gov;
    char idle_govs[128];
    char gpu[256];      /* /sys/class/drm/cardN/device */
    char gpu_hw[300];   /* its hwmon */
    long gpu_cap_min, gpu_cap_max, gpu_cap_def;
    int gpu_od;
    char k10[64], board[64], fans[64], fan_chip[32], nvme[64];
    char rapl[96];
    char nic[32];
    char ccd0_mask[64];
    char ethtool[64], tc[64];
} hw;

static int hwmon_by_name(const char *want, char *out, size_t sz)
{
    glob_t g;
    int found = -1;
    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &g))
        return -1;
    for (size_t i = 0; i < g.gl_pathc && found; i++) {
        char n[64];
        if (read_str(g.gl_pathv[i], n, sizeof n) > 0 && !strncmp(n, want, strlen(want))) {
            snprintf(out, sz, "%.*s", (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]),
                     g.gl_pathv[i]);
            found = 0;
        }
    }
    globfree(&g);
    return found;
}

static void find_gpu(void)
{
    glob_t g;
    long best = -1;
    hw.gpu[0] = 0;
    if (glob("/sys/class/drm/card[0-9]*/device/vendor", 0, NULL, &g))
        return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char v[16], dir[256], p[300], drv[300];
        if (read_str(g.gl_pathv[i], v, sizeof v) <= 0 || strcmp(v, "0x1002"))
            continue;
        snprintf(dir, sizeof dir, "%.*s", (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]),
                 g.gl_pathv[i]);
        snprintf(p, sizeof p, "%s/driver", dir);
        ssize_t n = readlink(p, drv, sizeof drv - 1);
        if (n <= 0)
            continue;
        drv[n] = 0;
        if (!strstr(drv, "/amdgpu"))
            continue;
        snprintf(p, sizeof p, "%s/mem_info_vram_total", dir);
        long vram = read_long(p);
        if (vram > best) {
            best = vram;
            snprintf(hw.gpu, sizeof hw.gpu, "%s", dir);
        }
    }
    globfree(&g);
    if (!hw.gpu[0])
        return;
    char pat[300];
    snprintf(pat, sizeof pat, "%s/hwmon/hwmon*", hw.gpu);
    if (!glob(pat, 0, NULL, &g)) {
        snprintf(hw.gpu_hw, sizeof hw.gpu_hw, "%s", g.gl_pathv[0]);
        globfree(&g);
    }
    char p[400];
    snprintf(p, sizeof p, "%s/power1_cap_min", hw.gpu_hw);
    hw.gpu_cap_min = read_long(p);
    snprintf(p, sizeof p, "%s/power1_cap_max", hw.gpu_hw);
    hw.gpu_cap_max = read_long(p);
    snprintf(p, sizeof p, "%s/power1_cap_default", hw.gpu_hw);
    hw.gpu_cap_def = read_long(p);
    snprintf(p, sizeof p, "%s/pp_od_clk_voltage", hw.gpu);
    hw.gpu_od = exists(p);
}

/* the wired interface that carries the default route, else the first one up */
static void find_nic(void)
{
    char route[4096] = "";
    hw.nic[0] = 0;
    FILE *f = fopen("/proc/net/route", "re");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char ifn[32];
        unsigned long dst;
        if (sscanf(line, "%31s %lx", ifn, &dst) == 2 && dst == 0) {
            snprintf(route, sizeof route, "%s", ifn);
            break;
        }
    }
    if (f)
        fclose(f);
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    char first[32] = "";
    while (d && (e = readdir(d))) {
        char p[300];
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "/sys/class/net/%s/device", e->d_name);
        if (!exists(p))
            continue;
        snprintf(p, sizeof p, "/sys/class/net/%s/wireless", e->d_name);
        if (exists(p))
            continue;
        snprintf(p, sizeof p, "/sys/class/net/%s/type", e->d_name);
        if (read_long(p) != 1)
            continue;
        if (!strcmp(e->d_name, route)) {
            snprintf(hw.nic, sizeof hw.nic, "%s", e->d_name);
            break;
        }
        if (!first[0])
            snprintf(first, sizeof first, "%s", e->d_name);
    }
    if (d)
        closedir(d);
    if (!hw.nic[0])
        snprintf(hw.nic, sizeof hw.nic, "%s", first);
}

/* CPUs that share cpu0's L3, as a hex mask for rps_cpus */
static void find_ccd0(void)
{
    long id0 = read_long("/sys/devices/system/cpu/cpu0/cache/index3/id");
    unsigned long long mask = 0;
    for (int c = 0; c < hw.ncpu && c < 64; c++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index3/id", c);
        if (read_long(p) == id0)
            mask |= 1ull << c;
    }
    if (mask)
        snprintf(hw.ccd0_mask, sizeof hw.ccd0_mask, "%llx", mask);
}

static void detect(void)
{
    memset(&hw, 0, sizeof hw);
    hw.ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
    hw.cpu_max_khz = read_long("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    hw.cpu_base_khz = read_long("/sys/devices/system/cpu/cpu0/cpufreq/base_frequency");
    if (hw.cpu_base_khz <= 0)
        hw.cpu_base_khz = read_long("/sys/devices/system/cpu/cpu0/cpufreq/amd_pstate_nominal_freq");
    hw.has_epp = exists("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference");
    hw.has_boost = exists("/sys/devices/system/cpu/cpufreq/boost");
    hw.has_idle_gov = read_str("/sys/devices/system/cpu/cpuidle/available_governors", hw.idle_govs,
                               sizeof hw.idle_govs) > 0;
    find_gpu();
    hwmon_by_name("k10temp", hw.k10, sizeof hw.k10);
    hwmon_by_name("gigabyte_wmi", hw.board, sizeof hw.board);
    if (hwmon_by_name("it8", hw.fans, sizeof hw.fans) && hwmon_by_name("nct6", hw.fans, sizeof hw.fans))
        hw.fans[0] = 0;
    else {
        char p[96];
        snprintf(p, sizeof p, "%s/name", hw.fans);
        read_str(p, hw.fan_chip, sizeof hw.fan_chip);
    }
    hwmon_by_name("nvme", hw.nvme, sizeof hw.nvme);
    if (exists("/sys/class/powercap/intel-rapl:0/energy_uj"))
        snprintf(hw.rapl, sizeof hw.rapl, "/sys/class/powercap/intel-rapl:0");
    find_nic();
    find_ccd0();
    const char *b = find_bin("ethtool");
    if (b)
        snprintf(hw.ethtool, sizeof hw.ethtool, "%s", b);
    b = find_bin("tc");
    if (b)
        snprintf(hw.tc, sizeof hw.tc, "%s", b);
}

/* --------------------------------------------------------------- state */

static struct {
    int base;          /* what the user picked */
    int active;        /* what is applied (auto can raise it) */
    char net[16];      /* auto | balanced | lowlatency | throughput */
    char net_active[16];
    int autosw;        /* automatic switching allowed */
    char workload[16]; /* idle | build | game */
    time_t workload_since;
} st = {STOCK_LEVEL, 0, "auto", "", 0, "idle", 0};

static void state_save(void)
{
    if (dry)
        return;
    FILE *f = fopen(STATE_FILE ".tmp", "we");
    if (!f)
        return;
    fprintf(f, "profile=%d\nnet=%s\nauto=%d\n", st.base, st.net, st.autosw);
    fclose(f);
    rename(STATE_FILE ".tmp", STATE_FILE);
}

static void state_load(void)
{
    FILE *f = fopen(STATE_FILE, "re");
    char line[128];
    while (f && fgets(line, sizeof line, f)) {
        int v;
        char s[16];
        if (sscanf(line, "profile=%d", &v) == 1)
            st.base = v;
        else if (sscanf(line, "net=%15s", s) == 1)
            snprintf(st.net, sizeof st.net, "%s", s);
        else if (sscanf(line, "auto=%d", &v) == 1)
            st.autosw = v != 0;
    }
    if (f)
        fclose(f);
}

static const struct level *level_get(int pct)
{
    for (int i = 0; i < NLEVELS; i++)
        if (levels[i].pct == pct)
            return &levels[i];
    return NULL;
}

static const struct netprof *net_get(const char *name)
{
    for (int i = 0; i < NNETPROFS; i++)
        if (!strcmp(netprofs[i].name, name))
            return &netprofs[i];
    return NULL;
}

/* -------------------------------------------------------------- apply */

static void for_cpus(const char *leaf, const char *val)
{
    for (int c = 0; c < hw.ncpu; c++) {
        char p[160];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/%s", c, leaf);
        if (exists(p))
            set_knob(p, val);
    }
}

static int gpu_mode_index(const char *name)
{
    char p[300], b[8192];
    snprintf(p, sizeof p, "%s/pp_power_profile_mode", hw.gpu);
    if (read_str(p, b, sizeof b) <= 0)
        return -1;
    for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n")) {
        int idx;
        char n[64];
        if (sscanf(l, " %d %63[A-Z0-9_]", &idx, n) == 2 && !strcmp(n, name))
            return idx;
    }
    return -1;
}

static void apply_level(const struct level *L)
{
    char v[64];
    /* governor first: amd-pstate refuses EPP changes under "performance" */
    for_cpus("scaling_governor", L->governor);
    if (hw.has_epp && strcmp(L->governor, "performance"))
        for_cpus("energy_performance_preference", L->epp);
    if (hw.has_boost) {
        /* boost off lowers scaling_max_freq by itself: record the max
         * clocks first (after boost, so stock restores boost first) */
        remember("/sys/devices/system/cpu/cpufreq/boost");
        for (int c = 0; c < hw.ncpu; c++) {
            char p[160];
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", c);
            remember(p);
        }
        set_knob("/sys/devices/system/cpu/cpufreq/boost", L->boost ? "1" : "0");
    }
    long khz = L->cpu_max_mhz ? L->cpu_max_mhz * 1000L : hw.cpu_max_khz;
    if (khz > hw.cpu_max_khz)
        khz = hw.cpu_max_khz;
    snprintf(v, sizeof v, "%ld", khz);
    for_cpus("scaling_max_freq", v);
    if (hw.has_idle_gov && L->idle_gov && strstr(hw.idle_govs, L->idle_gov))
        set_knob("/sys/devices/system/cpu/cpuidle/current_governor", L->idle_gov);

    if (hw.gpu[0]) {
        char p[400];
        if (hw.gpu_cap_def > 0) {
            long cap = hw.gpu_cap_def / 100 * L->gpu_cap_pct;
            cap = cap / 1000000 * 1000000; /* whole watts */
            if (cap < hw.gpu_cap_min)
                cap = hw.gpu_cap_min;
            if (cap > hw.gpu_cap_max)
                cap = hw.gpu_cap_max;
            snprintf(p, sizeof p, "%s/power1_cap", hw.gpu_hw);
            snprintf(v, sizeof v, "%ld", cap);
            set_knob(p, v);
        }
        int idx = gpu_mode_index(L->gpu_mode);
        if (idx >= 0) {
            snprintf(p, sizeof p, "%s/pp_power_profile_mode", hw.gpu);
            snprintf(v, sizeof v, "%d", idx);
            set_knob(p, v);
        }
    }
    logmsg("profile %d applied", L->pct);
}

static int eee_get(int *on)
{
    char out[2048];
    char *argv[] = {hw.ethtool, "--show-eee", hw.nic, NULL};
    if (!hw.ethtool[0] || run(argv, out, sizeof out))
        return -1;
    char *s = strstr(out, "EEE status:");
    if (!s)
        return -1;
    *on = strstr(s, "enabled") && strstr(s, "enabled") < strchr(s, '\n');
    return 0;
}

static void eee_set(int on)
{
    int cur;
    char key[64];
    if (eee_get(&cur))
        return;
    snprintf(key, sizeof key, "eee:%s", hw.nic);
    stock_add(key, cur ? "on" : "off");
    if (cur == on)
        return;
    char *argv[] = {hw.ethtool, "--set-eee", hw.nic, "eee", on ? "on" : "off", NULL};
    if (run(argv, NULL, 0))
        logmsg("ethtool --set-eee %s %s failed", hw.nic, on ? "on" : "off");
}

static void qdisc_set(const char *kind)
{
    char key[64], out[1024];
    char *show[] = {hw.tc, "qdisc", "show", "dev", hw.nic, "root", NULL};
    if (!hw.tc[0] || run(show, out, sizeof out))
        return;
    snprintf(key, sizeof key, "qdisc:%s", hw.nic);
    char cur[32] = "";
    sscanf(out, "qdisc %31s", cur);
    stock_add(key, cur);
    if (!strcmp(cur, kind))
        return;
    char *argv[] = {hw.tc, "qdisc", "replace", "dev", hw.nic, "root", (char *)kind, NULL};
    if (run(argv, NULL, 0))
        logmsg("tc qdisc replace %s %s failed", hw.nic, kind);
}

static void apply_net(const struct netprof *N)
{
    if (!hw.nic[0])
        return;
    char p[200], v[32];
    set_knob("/proc/sys/net/ipv4/tcp_congestion_control", N->cc);
    qdisc_set(N->qdisc);
    if (N->eee >= 0)
        eee_set(N->eee);
    else {
        /* leave = whatever it was before we ever touched it */
        char key[64];
        snprintf(key, sizeof key, "eee:%s", hw.nic);
        for (int i = 0; i < nstock; i++)
            if (!strcmp(stock[i].key, key))
                eee_set(!strcmp(stock[i].val, "on"));
    }
    glob_t g;
    snprintf(p, sizeof p, "/sys/class/net/%s/queues/rx-*/rps_cpus", hw.nic);
    if (!glob(p, 0, NULL, &g)) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            set_knob(g.gl_pathv[i], N->rps && hw.ccd0_mask[0] ? hw.ccd0_mask : "0");
        globfree(&g);
    }
    /* interrupt batching: gro_flush_timeout and napi_defer_hard_irqs go
     * together (r8169 sets 20 us / 1); 0 means an interrupt per packet */
    static const char *const irq_leaf[] = {"gro_flush_timeout", "napi_defer_hard_irqs"};
    for (int k = 0; k < 2; k++) {
        snprintf(p, sizeof p, "/sys/class/net/%s/%s", hw.nic, irq_leaf[k]);
        if (!exists(p))
            continue;
        if (N->gro_flush_us >= 0) {
            snprintf(v, sizeof v, "%d", k == 0 ? N->gro_flush_us * 1000 : N->gro_flush_us ? 1 : 0);
            set_knob(p, v);
        } else {
            for (int i = 0; i < nstock; i++)
                if (!strcmp(stock[i].key, p))
                    set_knob(p, stock[i].val);
        }
    }
    snprintf(st.net_active, sizeof st.net_active, "%s", N->name);
    logmsg("network %s applied on %s", N->name, hw.nic);
}

static void apply_all(int pct)
{
    const struct level *L = level_get(pct);
    if (!L)
        return;
    apply_level(L);
    st.active = pct;
    const struct netprof *N = net_get(strcmp(st.net, "auto") ? st.net : L->net);
    if (N && strcmp(st.net_active, N->name))
        apply_net(N);
}

/* put back everything we ever changed */
static void revert_stock(void)
{
    /* governor before EPP again */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nstock; i++) {
            const char *k = stock[i].key;
            int gov = strstr(k, "scaling_governor") != NULL;
            if (pass == 0 ? !gov : gov)
                continue;
            if (!strncmp(k, "eee:", 4)) {
                if (!strcmp(k + 4, hw.nic))
                    eee_set(!strcmp(stock[i].val, "on"));
            } else if (!strncmp(k, "qdisc:", 6)) {
                if (!strcmp(k + 6, hw.nic) && hw.tc[0]) {
                    /* deleting the root qdisc brings back the kernel default */
                    char *argv[] = {hw.tc, "qdisc", "del", "dev", hw.nic, "root", NULL};
                    run(argv, NULL, 0);
                }
            } else if (k[0] == '/') {
                char cur[200];
                current_value(k, cur, sizeof cur);
                if (strcmp(cur, stock[i].val))
                    raw_write(k, stock[i].val);
            }
        }
    st.active = 0;
    st.net_active[0] = 0;
    logmsg("stock settings restored (%d values)", nstock);
}

/* ------------------------------------------------------------- monitor */

static struct {
    double cpu_mhz_avg, cpu_mhz_max, cpu_use, cpu_w;
    double tctl, tccd1, tccd2;
    double gpu_mhz, gpu_mem_mhz, gpu_use, gpu_edge, gpu_junc, gpu_mem_t, gpu_w, gpu_cap_w;
    double vram_used, vram_total, gpu_fan;
    double ram_used, ram_total, swap_used;
    double nvme_t;
    double fan[6];
    char fan_label[6][24];
    int nfan;
    double net_rx, net_tx;
    long net_speed;
    double board_t;
} mon;

static unsigned long long prev_busy, prev_total, prev_rapl, prev_rx, prev_tx;
static struct timespec prev_t;

static double hw_temp(const char *dir, const char *label)
{
    char p[160], l[32];
    if (!dir[0])
        return -1;
    for (int i = 1; i <= 8; i++) {
        snprintf(p, sizeof p, "%s/temp%d_label", dir, i);
        if (read_str(p, l, sizeof l) > 0 && !strcmp(l, label)) {
            snprintf(p, sizeof p, "%s/temp%d_input", dir, i);
            return read_long(p) / 1000.0;
        }
    }
    return -1;
}

/* "1: 2400Mhz *" line of pp_dpm_* */
static double dpm_current(const char *leaf)
{
    char p[300], b[1024];
    snprintf(p, sizeof p, "%s/%s", hw.gpu, leaf);
    if (read_str(p, b, sizeof b) <= 0)
        return -1;
    for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n"))
        if (strchr(l, '*')) {
            char *c = strchr(l, ':');
            return c ? strtod(c + 1, NULL) : -1;
        }
    return -1;
}

static void sample(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = (now.tv_sec - prev_t.tv_sec) + (now.tv_nsec - prev_t.tv_nsec) / 1e9;
    int first = prev_t.tv_sec == 0;
    prev_t = now;

    long sum = 0, mx = 0, n = 0;
    for (int c = 0; c < hw.ncpu; c++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        long f = read_long(p);
        if (f > 0) {
            sum += f;
            n++;
            if (f > mx)
                mx = f;
        }
    }
    mon.cpu_mhz_avg = n ? sum / (double)n / 1000 : 0;
    mon.cpu_mhz_max = mx / 1000.0;

    FILE *f = fopen("/proc/stat", "re");
    if (f) {
        unsigned long long u, ni, sy, id, io, irq, sirq, steal;
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &ni, &sy, &id, &io, &irq,
                   &sirq, &steal) == 8) {
            unsigned long long busy = u + ni + sy + irq + sirq + steal, total = busy + id + io;
            if (!first && total > prev_total)
                mon.cpu_use = 100.0 * (busy - prev_busy) / (total - prev_total);
            prev_busy = busy;
            prev_total = total;
        }
        fclose(f);
    }

    if (hw.rapl[0]) {
        char p[160];
        snprintf(p, sizeof p, "%s/energy_uj", hw.rapl);
        long long e = read_ll(p);
        snprintf(p, sizeof p, "%s/max_energy_range_uj", hw.rapl);
        long long range = read_ll(p);
        if (e >= 0 && !first && dt > 0) {
            long long d = e - (long long)prev_rapl;
            if (d < 0 && range > 0)
                d += range;
            mon.cpu_w = d / 1e6 / dt;
        }
        prev_rapl = (unsigned long long)e;
    }

    mon.tctl = hw_temp(hw.k10, "Tctl");
    mon.tccd1 = hw_temp(hw.k10, "Tccd1");
    mon.tccd2 = hw_temp(hw.k10, "Tccd2");

    if (hw.gpu[0]) {
        char p[400];
        mon.gpu_mhz = dpm_current("pp_dpm_sclk");
        mon.gpu_mem_mhz = dpm_current("pp_dpm_mclk");
        snprintf(p, sizeof p, "%s/gpu_busy_percent", hw.gpu);
        mon.gpu_use = read_long(p);
        mon.gpu_edge = hw_temp(hw.gpu_hw, "edge");
        mon.gpu_junc = hw_temp(hw.gpu_hw, "junction");
        mon.gpu_mem_t = hw_temp(hw.gpu_hw, "mem");
        snprintf(p, sizeof p, "%s/power1_average", hw.gpu_hw);
        long w = read_long(p);
        if (w < 0) {
            snprintf(p, sizeof p, "%s/power1_input", hw.gpu_hw);
            w = read_long(p);
        }
        mon.gpu_w = w / 1e6;
        snprintf(p, sizeof p, "%s/power1_cap", hw.gpu_hw);
        mon.gpu_cap_w = read_long(p) / 1e6;
        snprintf(p, sizeof p, "%s/mem_info_vram_used", hw.gpu);
        mon.vram_used = read_ll(p) / 1048576.0;
        snprintf(p, sizeof p, "%s/mem_info_vram_total", hw.gpu);
        mon.vram_total = read_ll(p) / 1048576.0;
        snprintf(p, sizeof p, "%s/fan1_input", hw.gpu_hw);
        mon.gpu_fan = read_long(p);
    }

    f = fopen("/proc/meminfo", "re");
    if (f) {
        char line[128];
        long tot = 0, avail = 0, stot = 0, sfree = 0, v;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %ld", &v) == 1) tot = v;
            else if (sscanf(line, "MemAvailable: %ld", &v) == 1) avail = v;
            else if (sscanf(line, "SwapTotal: %ld", &v) == 1) stot = v;
            else if (sscanf(line, "SwapFree: %ld", &v) == 1) sfree = v;
        }
        fclose(f);
        mon.ram_total = tot / 1024.0;
        mon.ram_used = (tot - avail) / 1024.0;
        mon.swap_used = (stot - sfree) / 1024.0;
    }

    mon.nvme_t = hw_temp(hw.nvme, "Composite");
    mon.board_t = -1;
    if (hw.board[0]) {
        char p[128];
        snprintf(p, sizeof p, "%s/temp1_input", hw.board);
        mon.board_t = read_long(p) / 1000.0;
    }

    mon.nfan = 0;
    if (hw.fans[0])
        for (int i = 1; i <= 6 && mon.nfan < 6; i++) {
            char p[128];
            snprintf(p, sizeof p, "%s/fan%d_input", hw.fans, i);
            long rpm = read_long(p);
            if (rpm <= 0)
                continue;
            mon.fan[mon.nfan] = rpm;
            snprintf(p, sizeof p, "%s/fan%d_label", hw.fans, i);
            if (read_str(p, mon.fan_label[mon.nfan], sizeof mon.fan_label[0]) <= 0)
                snprintf(mon.fan_label[mon.nfan], sizeof mon.fan_label[0], "fan%d", i);
            mon.nfan++;
        }

    if (hw.nic[0]) {
        char p[160];
        snprintf(p, sizeof p, "/sys/class/net/%s/statistics/rx_bytes", hw.nic);
        unsigned long long rx = (unsigned long long)read_ll(p);
        snprintf(p, sizeof p, "/sys/class/net/%s/statistics/tx_bytes", hw.nic);
        unsigned long long tx = (unsigned long long)read_ll(p);
        if (!first && dt > 0) {
            mon.net_rx = (rx - prev_rx) / dt / 1000;
            mon.net_tx = (tx - prev_tx) / dt / 1000;
        }
        prev_rx = rx;
        prev_tx = tx;
        snprintf(p, sizeof p, "/sys/class/net/%s/speed", hw.nic);
        mon.net_speed = read_long(p);
    }
}

/* ------------------------------------------------------------ workload
 * Games and builds are recognised by process names. Auto only ever raises
 * the level above what the user picked, and only when they turned it on. */

static const char *const game_procs[] = {"wine64-preload", "wine-preload", "wineserver",
                                         "gamescope", "dolphin-emu", "rpcs3", "pcsx2-qt",
                                         NULL};
static const char *const build_procs[] = {"cc1", "cc1plus", "clang", "clang++", "rustc", "ld.lld",
                                          "mold", "ld", "javac", "go", NULL};

static int in_list(const char *s, const char *const *l)
{
    for (; *l; l++)
        if (!strcmp(s, *l))
            return 1;
    return 0;
}

static const char *detect_workload(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int builds = 0, game = 0, exe = 0;
    while (d && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char p[64], comm[32];
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        if (read_str(p, comm, sizeof comm) <= 0)
            continue;
        if (in_list(comm, build_procs))
            builds++;
        else if (in_list(comm, game_procs))
            game = 1;
        else {
            size_t n = strlen(comm);
            if (n > 4 && !strcasecmp(comm + n - 4, ".exe"))
                exe = 1;
        }
    }
    if (d)
        closedir(d);
    /* a Windows game under wine/proton plus a busy GPU, or an emulator */
    if (game || (exe && mon.gpu_use > 50))
        return "game";
    if (builds >= 4)
        return "build";
    return "idle";
}

static void auto_tick(void)
{
    const char *w = detect_workload();
    time_t now = time(NULL);
    if (strcmp(w, st.workload)) {
        snprintf(st.workload, sizeof st.workload, "%s", w);
        st.workload_since = now;
        logmsg("workload: %s", w);
    }
    if (!st.base) /* stock: hands off */
        return;
    if (!st.autosw) {
        if (st.active != st.base)
            apply_all(st.base);
        return;
    }
    int want = st.base;
    if (strcmp(st.workload, "idle"))
        want = 100;
    /* go up right away, come back down after 30 s of quiet */
    if (want > st.active || (want < st.active && now - st.workload_since >= 30))
        apply_all(want > st.base ? want : st.base);
}

/* -------------------------------------------------------------- socket */

static int allowed_gid = -1;

static int peer_ok(int fd)
{
    struct ucred cr;
    socklen_t len = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len))
        return 0;
    if (cr.uid == 0 || cr.uid == geteuid())
        return 1;
    if (allowed_gid < 0)
        return 0;
    struct passwd *pw = getpwuid(cr.uid);
    if (!pw)
        return 0;
    gid_t groups[128];
    int ng = 128;
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ng) < 0)
        return 0;
    for (int i = 0; i < ng; i++)
        if ((int)groups[i] == allowed_gid)
            return 1;
    return 0;
}

struct client {
    int fd;
    char buf[PROTO_MAX_LINE];
    size_t len;
};
static struct client clients[MAX_CLIENTS];

static void out(struct strbuf *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->s + o->len, sizeof o->s - o->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len += (size_t)n < sizeof o->s - o->len ? (size_t)n : sizeof o->s - o->len - 1;
}

static void reply_status(struct strbuf *o)
{
    out(o, "ok\nprofile=%d\nactive=%d\nnet=%s\nnet_active=%s\nauto=%d\nworkload=%s\n", st.base,
        st.active, st.net, st.net_active, st.autosw, st.workload);
    const struct level *L = level_get(st.active ? st.active : st.base);
    if (L)
        out(o, "summary=%s\n", L->summary);
    for (int i = 0; i < NLEVELS; i++)
        out(o, "level=%d|%s\n", levels[i].pct, levels[i].summary);
    for (int i = 0; i < NNETPROFS; i++)
        out(o, "netprof=%s|%s\n", netprofs[i].name, netprofs[i].summary);
}

static void reply_caps(struct strbuf *o)
{
    out(o, "ok\ncpus=%d\ncpu_max_mhz=%ld\ncpu_epp=%d\ncpu_boost=%d\nidle_governors=%s\n", hw.ncpu,
        hw.cpu_max_khz / 1000, hw.has_epp, hw.has_boost, hw.has_idle_gov ? hw.idle_govs : "");
    out(o, "gpu=%s\ngpu_cap_w=%ld..%ld default %ld\ngpu_overdrive=%d\n",
        hw.gpu[0] ? strrchr(hw.gpu, '/') ? hw.gpu : hw.gpu : "none", hw.gpu_cap_min / 1000000,
        hw.gpu_cap_max / 1000000, hw.gpu_cap_def / 1000000, hw.gpu_od);
    out(o, "rapl=%d\ncpu_sensors=%s\nboard_sensors=%s\nfan_sensors=%s\nnvme_sensor=%s\n",
        hw.rapl[0] != 0, hw.k10[0] ? "k10temp" : "none", hw.board[0] ? "gigabyte_wmi" : "none",
        hw.fans[0] ? hw.fan_chip : "none", hw.nvme[0] ? "yes" : "none");
    out(o, "nic=%s\nethtool=%d\ntc=%d\nccd0_mask=%s\nfan_control=bios\n", hw.nic[0] ? hw.nic : "none",
        hw.ethtool[0] != 0, hw.tc[0] != 0, hw.ccd0_mask);
}

static void reply_monitor(struct strbuf *o)
{
    out(o, "ok\ncpu_mhz=%.0f\ncpu_mhz_max=%.0f\ncpu_use=%.1f\ncpu_w=%.1f\ncpu_temp=%.1f\n",
        mon.cpu_mhz_avg, mon.cpu_mhz_max, mon.cpu_use, mon.cpu_w, mon.tctl);
    out(o, "ccd1_temp=%.1f\nccd2_temp=%.1f\n", mon.tccd1, mon.tccd2);
    out(o, "gpu_mhz=%.0f\ngpu_mem_mhz=%.0f\ngpu_use=%.0f\ngpu_temp=%.1f\ngpu_hot=%.1f\n", mon.gpu_mhz,
        mon.gpu_mem_mhz, mon.gpu_use, mon.gpu_edge, mon.gpu_junc);
    out(o, "gpu_mem_temp=%.1f\ngpu_w=%.1f\ngpu_cap_w=%.0f\nvram_used=%.0f\nvram_total=%.0f\n",
        mon.gpu_mem_t, mon.gpu_w, mon.gpu_cap_w, mon.vram_used, mon.vram_total);
    out(o, "gpu_fan=%.0f\nram_used=%.0f\nram_total=%.0f\nswap_used=%.0f\nnvme_temp=%.1f\n",
        mon.gpu_fan, mon.ram_used, mon.ram_total, mon.swap_used, mon.nvme_t);
    out(o, "board_temp=%.1f\nnet_if=%s\nnet_speed=%ld\nnet_rx_kbs=%.1f\nnet_tx_kbs=%.1f\n",
        mon.board_t, hw.nic, mon.net_speed, mon.net_rx, mon.net_tx);
    for (int i = 0; i < mon.nfan; i++)
        out(o, "fan=%s|%.0f\n", mon.fan_label[i], mon.fan[i]);
    out(o, "workload=%s\nactive=%d\n", st.workload, st.active);
}

static void handle(struct client *c, char *line, uid_t uid)
{
    struct strbuf o = {.len = 0};
    char a[32] = "", b[32] = "";
    int n = sscanf(line, "%31s %31s", a, b);
    if (n >= 1 && !strcmp(a, "status"))
        reply_status(&o);
    else if (n >= 1 && !strcmp(a, "caps"))
        reply_caps(&o);
    else if (n >= 1 && !strcmp(a, "monitor"))
        reply_monitor(&o);
    else if (n == 2 && !strcmp(a, "profile")) {
        int pct = atoi(b);
        if (!level_get(pct))
            out(&o, "err no such level %s\n", b);
        else {
            st.base = pct;
            apply_all(pct);
            state_save();
            logmsg("uid %d set profile %d", (int)uid, pct);
            out(&o, "ok\n");
        }
    } else if (n == 2 && !strcmp(a, "net")) {
        if (strcmp(b, "auto") && !net_get(b))
            out(&o, "err no such network profile %s\n", b);
        else {
            snprintf(st.net, sizeof st.net, "%s", b);
            st.net_active[0] = 0;
            apply_all(st.active ? st.active : st.base);
            state_save();
            logmsg("uid %d set net %s", (int)uid, b);
            out(&o, "ok\n");
        }
    } else if (n == 2 && !strcmp(a, "auto")) {
        st.autosw = !strcmp(b, "on");
        state_save();
        logmsg("uid %d set auto %s", (int)uid, st.autosw ? "on" : "off");
        out(&o, "ok\n");
    } else if (n >= 1 && !strcmp(a, "stock")) {
        revert_stock();
        st.base = 0;
        snprintf(st.net, sizeof st.net, "auto");
        st.autosw = 0;
        state_save();
        logmsg("uid %d reverted to stock", (int)uid);
        out(&o, "ok\n");
    } else
        out(&o, "err unknown command\n");
    out(&o, ".\n");
    ssize_t w = write(c->fd, o.s, o.len);
    (void)w;
}

static int listen_sock(void)
{
    if (!dry) {
        mkdir(PROTO_DIR, 0750);
        if (allowed_gid >= 0 && chown(PROTO_DIR, 0, (gid_t)allowed_gid))
            logmsg("chown %s: %s", PROTO_DIR, strerror(errno));
        chmod(PROTO_DIR, 0750);
    }
    unlink(sock_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock_path);
    mode_t old = umask(0117);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, 8)) {
        logmsg("socket: %s", strerror(errno));
        exit(1);
    }
    umask(old);
    if (!dry && allowed_gid >= 0 && chown(sock_path, 0, (gid_t)allowed_gid))
        logmsg("chown socket: %s", strerror(errno));
    return fd;
}

static void on_sig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
    const char *group = "optimzr";
    int opt;
    while ((opt = getopt(argc, argv, "fg:ns:")) != -1) {
        if (opt == 'f')
            foreground = 1;
        else if (opt == 'g')
            group = optarg;
        else if (opt == 'n')
            dry = foreground = 1;
        else if (opt == 's')
            sock_path = optarg;
        else {
            fprintf(stderr, "usage: optimzrd [-f] [-g group] [-n] [-s socket]\n");
            return 2;
        }
    }
    if (geteuid() && !dry) {
        fprintf(stderr, "optimzrd must run as root\n");
        return 1;
    }
    umask(022);
    if (!dry)
        mkdir(STATE_DIR, 0700);
    if (!foreground)
        logf = fopen(LOG_FILE, "ae");
    struct group *gr = getgrnam(group);
    allowed_gid = gr ? (int)gr->gr_gid : -1;
    if (!gr)
        logmsg("group %s not found, only root can connect", group);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    detect();
    stock_load();
    state_load();
    if (st.base && !level_get(st.base))
        st.base = STOCK_LEVEL;
    if (strcmp(st.net, "auto") && !net_get(st.net))
        snprintf(st.net, sizeof st.net, "auto");
    logmsg("start: %d cpus, gpu %s, nic %s, profile %d, net %s, auto %d", hw.ncpu,
           hw.gpu[0] ? hw.gpu : "none", hw.nic[0] ? hw.nic : "none", st.base, st.net, st.autosw);
    if (st.base)
        apply_all(st.base);
    sample();

    int lfd = listen_sock();
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    int ticks = 0;

    while (!quit) {
        struct pollfd pf[MAX_CLIENTS + 1];
        int map[MAX_CLIENTS + 1], np = 0;
        pf[np] = (struct pollfd){lfd, POLLIN, 0};
        map[np++] = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd >= 0) {
                pf[np] = (struct pollfd){clients[i].fd, POLLIN, 0};
                map[np++] = i;
            }
        int r = poll(pf, (nfds_t)np, 250);
        if (r < 0 && errno != EINTR)
            break;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 >= 1000) {
            last = now;
            sample();
            if (++ticks % 2 == 0)
                auto_tick();
        }
        if (r <= 0)
            continue;

        if (pf[0].revents & POLLIN) {
            int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (cfd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS && slot < 0; i++)
                    if (clients[i].fd < 0)
                        slot = i;
                if (slot < 0 || !peer_ok(cfd)) {
                    const char *m = slot < 0 ? "err busy\n.\n" : "err not allowed\n.\n";
                    ssize_t w = write(cfd, m, strlen(m));
                    (void)w;
                    close(cfd);
                } else {
                    clients[slot].fd = cfd;
                    clients[slot].len = 0;
                }
            }
        }
        for (int k = 1; k < np; k++) {
            if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            struct client *c = &clients[map[k]];
            ssize_t n = read(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len);
            if (n <= 0) {
                close(c->fd);
                c->fd = -1;
                continue;
            }
            c->len += (size_t)n;
            c->buf[c->len] = 0;
            char *nl;
            while ((nl = memchr(c->buf, '\n', c->len))) {
                *nl = 0;
                struct ucred cr;
                socklen_t cl = sizeof cr;
                getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl);
                handle(c, c->buf, cr.uid);
                size_t used = (size_t)(nl - c->buf) + 1;
                memmove(c->buf, nl + 1, c->len - used);
                c->len -= used;
            }
            if (c->len >= sizeof c->buf - 1) { /* line too long */
                close(c->fd);
                c->fd = -1;
            }
        }
    }
    logmsg("stopping");
    unlink(sock_path);
    revert_stock();
    return 0;
}
