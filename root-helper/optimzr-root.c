/*
 * optimzr-root: the only thing the optimzr-gentoo project may run as root.
 *
 * Every command is a fixed action with checked arguments. No shell, no
 * arbitrary commands, clean environment, absolute binary paths.
 * Runtime changes (sysfs, sysctl) keep the first-seen original value in
 * /var/lib/optimzr/runtime-orig so `revert-runtime` (or a reboot) undoes them.
 * Installed files are backed up to /var/lib/optimzr/backup/<stamp>/ first.
 *
 * Built by setup.sh with -DPROJECT_DIR="..." and installed as
 * /usr/local/sbin/optimzr-root, root:root 0755.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <ftw.h>
#include <glob.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PROJECT_DIR
#error "build with -DPROJECT_DIR=\"/home/<user>/src/optimzr-gentoo\""
#endif

#define STATE_DIR  "/var/lib/optimzr"
#define ORIG_FILE  STATE_DIR "/runtime-orig"
#define BACKUP_DIR STATE_DIR "/backup"
#define LOG_FILE   "/var/log/optimzr-root.log"
#define SELF       "/usr/local/sbin/optimzr-root"

static int dry;
static char stamp[64];

/* ------------------------------------------------------------------ util */

static void logmsg(const char *fmt, ...)
{
    if (dry)
        return;
    FILE *f = fopen(LOG_FILE, "a");
    if (!f)
        return;
    char t[32];
    time_t now = time(NULL);
    strftime(t, sizeof t, "%F %T", localtime(&now));
    const char *u = getenv("SUDO_USER");
    fprintf(f, "%s [%s] ", t, u ? u : "?");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void die(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "optimzr-root: %s\n", buf);
    logmsg("REFUSED %s", buf);
    exit(1);
}

static int in_list(const char *s, const char *const *list)
{
    for (; *list; list++)
        if (!strcmp(s, *list))
            return 1;
    return 0;
}

/* plain argument word: letters, digits and a few punctuation marks */
static int safe_word(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 128)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("._:/=,+@-", *s))
            return 0;
    return 1;
}

/* value written to sysfs/procfs: printable ASCII, no newlines */
static int safe_value(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 512)
        return 0;
    for (; *s; s++)
        if (*s < 0x20 || *s > 0x7e)
            return 0;
    return 1;
}

static int has_dotdot(const char *p)
{
    return strstr(p, "/../") || strstr(p, "/./") || strstr(p, "//") ||
           (strlen(p) >= 3 && !strcmp(p + strlen(p) - 3, "/.."));
}

static const char *find_bin(const char *name)
{
    static const char *dirs[] = {"/usr/local/sbin", "/usr/local/bin", "/usr/sbin",
                                 "/usr/bin", "/sbin", "/bin", NULL};
    static char buf[PATH_MAX];
    for (int i = 0; dirs[i]; i++) {
        snprintf(buf, sizeof buf, "%s/%s", dirs[i], name);
        if (access(buf, X_OK) == 0)
            return buf;
    }
    return NULL;
}

static char *const clean_env[] = {
    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
    "HOME=/root", "LANG=C.UTF-8", "LC_MESSAGES=C.UTF-8", "TERM=dumb", NULL};

/* Run argv[0] (looked up in the fixed dirs) with a clean environment.
 * Returns the exit status, or -1 if the binary is missing. */
static int run_env(char *const argv[], char *const env[])
{
    char line[2048] = "";
    for (int i = 0; argv[i]; i++) {
        strncat(line, argv[i], sizeof line - strlen(line) - 2);
        strncat(line, " ", sizeof line - strlen(line) - 1);
    }
    const char *bin = find_bin(argv[0]);
    if (!bin) {
        fprintf(stderr, "optimzr-root: %s is not installed\n", argv[0]);
        return -1;
    }
    if (dry) {
        printf("would run: %s\n", line);
        return 0;
    }
    logmsg("run %s", line);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", bin);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        execve(path, argv, env);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    logmsg("exit %d", rc);
    return rc;
}

static int run(char *const argv[]) { return run_env(argv, clean_env); }

static int read_small(const char *path, char *buf, size_t sz)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return (int)n;
}

static void mkdir_p(const char *dir, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    mkdir(tmp, mode);
}

/* -------------------------------------------------- runtime orig values */

static int orig_known(const char *key)
{
    FILE *f = fopen(ORIG_FILE, "r");
    if (!f)
        return 0;
    char line[1024];
    size_t kl = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '\t') {
            found = 1;
            break;
        }
    fclose(f);
    return found;
}

static void orig_save(const char *key, const char *value)
{
    if (dry || orig_known(key))
        return;
    mkdir_p(STATE_DIR, 0700);
    FILE *f = fopen(ORIG_FILE, "a");
    if (!f)
        die("cannot write %s: %s", ORIG_FILE, strerror(errno));
    fprintf(f, "%s\t%s\n", key, value);
    fclose(f);
}

/* Turn what a sysfs file reads back into something that can be written
 * back to restore it. Returns NULL if it cannot be restored that way. */
static const char *restorable(const char *path, char *buf)
{
    const char *base = strrchr(path, '/') + 1;
    if (!strcmp(base, "pp_power_profile_mode")) {
        /* table; the active line has a '*' after the name */
        char *star = strchr(buf, '*');
        if (!star)
            return NULL;
        char *ls = star;
        while (ls > buf && ls[-1] != '\n')
            ls--;
        while (*ls == ' ')
            ls++;
        static char idx[16];
        int n = 0;
        while (isdigit((unsigned char)ls[n]) && n < 4)
            n++;
        if (!n)
            return NULL;
        snprintf(idx, sizeof idx, "%.*s", n, ls);
        return idx;
    }
    if (!strcmp(base, "pp_od_clk_voltage") || strstr(path, "/gpu_od/"))
        return "@od-reset";
    if (!strncmp(base, "pp_dpm_", 7))
        return "@skip"; /* power_dpm_force_performance_level=auto undoes it */
    if (!strcmp(base, "scaling_governor") || !strcmp(base, "energy_performance_preference") ||
        !strcmp(base, "current_governor") || !strcmp(base, "scheduler") ||
        !strcmp(base, "enabled") || !strcmp(base, "defrag") || !strcmp(base, "shmem_enabled") ||
        !strcmp(base, "policy")) {
        /* "a [b] c" style: keep the bracketed one */
        char *l = strchr(buf, '['), *r = l ? strchr(l, ']') : NULL;
        if (l && r) {
            *r = 0;
            return l + 1;
        }
    }
    buf[strcspn(buf, "\n")] = 0;
    if (strchr(buf, '\n') || !*buf)
        return NULL;
    return buf;
}

/* --------------------------------------------------------- sysfs writes */

static const char *const sys_patterns[] = {
    /* CPU */
    "/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor",
    "/sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference",
    "/sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq",
    "/sys/devices/system/cpu/cpu*/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu*/cpufreq/boost",
    "/sys/devices/system/cpu/cpufreq/policy*/scaling_governor",
    "/sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference",
    "/sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq",
    "/sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq",
    "/sys/devices/system/cpu/cpufreq/policy*/boost",
    "/sys/devices/system/cpu/cpufreq/boost",
    "/sys/devices/system/cpu/amd_pstate/status",
    "/sys/devices/system/cpu/cpu*/cpuidle/state*/disable",
    "/sys/devices/system/cpu/cpuidle/current_governor",
    "/sys/devices/system/cpu/smt/control",
    /* GPU (amdgpu) */
    "/sys/class/drm/card*/device/power_dpm_force_performance_level",
    "/sys/class/drm/card*/device/pp_power_profile_mode",
    "/sys/class/drm/card*/device/pp_od_clk_voltage",
    "/sys/class/drm/card*/device/pp_dpm_sclk",
    "/sys/class/drm/card*/device/pp_dpm_mclk",
    "/sys/class/drm/card*/device/pp_dpm_fclk",
    "/sys/class/drm/card*/device/pp_dpm_socclk",
    "/sys/class/drm/card*/device/gpu_od/fan_ctrl/*",
    "/sys/class/drm/card*/device/hwmon/hwmon*/power1_cap",
    "/sys/class/drm/card*/device/hwmon/hwmon*/pwm1",
    "/sys/class/drm/card*/device/hwmon/hwmon*/pwm1_enable",
    /* fans on the Super I/O (checked against hwmon name below) */
    "/sys/class/hwmon/hwmon*/pwm[1-9]",
    "/sys/class/hwmon/hwmon*/pwm[1-9]_enable",
    "/sys/class/hwmon/hwmon*/pwm[1-9]_auto_*",
    /* storage */
    "/sys/block/nvme*n*/queue/scheduler",
    "/sys/block/nvme*n*/queue/read_ahead_kb",
    "/sys/block/nvme*n*/queue/nr_requests",
    "/sys/block/nvme*n*/queue/wbt_lat_usec",
    "/sys/block/nvme*n*/queue/rq_affinity",
    "/sys/block/nvme*n*/queue/nomerges",
    "/sys/block/nvme*n*/queue/iostats",
    "/sys/block/nvme*n*/queue/add_random",
    /* memory */
    "/sys/kernel/mm/transparent_hugepage/enabled",
    "/sys/kernel/mm/transparent_hugepage/defrag",
    "/sys/kernel/mm/transparent_hugepage/shmem_enabled",
    "/sys/kernel/mm/transparent_hugepage/khugepaged/defrag",
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan",
    "/sys/kernel/mm/lru_gen/enabled",
    "/sys/kernel/mm/lru_gen/min_ttl_ms",
    "/sys/module/zswap/parameters/enabled",
    "/sys/module/zswap/parameters/compressor",
    "/sys/module/zswap/parameters/max_pool_percent",
    "/sys/module/zswap/parameters/shrinker_enabled",
    /* network (wired NICs only, checked below) */
    "/sys/class/net/*/queues/rx-*/rps_cpus",
    "/sys/class/net/*/queues/rx-*/rps_flow_cnt",
    "/sys/class/net/*/queues/tx-*/xps_cpus",
    "/sys/class/net/*/tx_queue_len",
    "/sys/class/net/*/gro_flush_timeout",
    "/sys/class/net/*/napi_defer_hard_irqs",
    "/sys/class/net/*/threaded",
    "/sys/class/net/*/device/power/control",
    "/sys/module/pcie_aspm/parameters/policy",
    /* interrupts */
    "/proc/irq/*/smp_affinity_list",
    NULL};

static int wired_nic(const char *ifname)
{
    char p[PATH_MAX];
    struct stat st;
    if (!safe_word(ifname) || strchr(ifname, '/'))
        return 0;
    snprintf(p, sizeof p, "/sys/class/net/%s/device", ifname);
    if (stat(p, &st))
        return 0;
    snprintf(p, sizeof p, "/sys/class/net/%s/wireless", ifname);
    if (!stat(p, &st))
        return 0;
    snprintf(p, sizeof p, "/sys/class/net/%s/phy80211", ifname);
    return stat(p, &st) != 0;
}

static void check_sys_path(const char *path)
{
    if (has_dotdot(path))
        die("path with ./.. or //: %s", path);
    int ok = 0;
    for (int i = 0; sys_patterns[i]; i++)
        if (!fnmatch(sys_patterns[i], path, FNM_PATHNAME)) {
            ok = 1;
            break;
        }
    if (!ok)
        die("not an allowed path: %s", path);

    if (!strncmp(path, "/sys/class/hwmon/", 17)) {
        char dir[PATH_MAX], name[64];
        snprintf(dir, sizeof dir, "%s", path);
        *strrchr(dir, '/') = 0;
        strncat(dir, "/name", sizeof dir - strlen(dir) - 1);
        if (read_small(dir, name, sizeof name) < 0)
            die("cannot read %s", dir);
        name[strcspn(name, "\n")] = 0;
        if (strcmp(name, "it87") && strcmp(name, "amdgpu") && strncmp(name, "nct6", 4))
            die("fan writes only on it87/nct6xxx/amdgpu, %s is %s", path, name);
    }
    if (!strncmp(path, "/sys/class/net/", 15)) {
        char ifn[64];
        const char *s = path + 15, *e = strchr(s, '/');
        snprintf(ifn, sizeof ifn, "%.*s", (int)(e - s), s);
        if (!wired_nic(ifn))
            die("%s is not a wired NIC", ifn);
    }
    char real[PATH_MAX];
    if (!realpath(path, real))
        die("%s: %s", path, strerror(errno));
    if (strncmp(real, "/sys/", 5) && strncmp(real, "/proc/irq/", 10))
        die("%s resolves outside /sys", path);
}

static int write_file_value(const char *path, const char *value)
{
    if (dry) {
        printf("would write %s <- %s\n", path, value);
        return 0;
    }
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "optimzr-root: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    char buf[600];
    int n = snprintf(buf, sizeof buf, "%s\n", value);
    ssize_t w = write(fd, buf, n);
    int err = errno;
    close(fd);
    if (w != n) {
        fprintf(stderr, "optimzr-root: write %s <- %s: %s\n", path, value, strerror(err));
        logmsg("write FAILED %s <- %s: %s", path, value, strerror(err));
        return 1;
    }
    logmsg("write %s <- %s", path, value);
    return 0;
}

static void remember(const char *path)
{
    if (orig_known(path))
        return;
    char buf[8192];
    if (read_small(path, buf, sizeof buf) >= 0) {
        const char *r = restorable(path, buf);
        orig_save(path, r ? r : "@skip");
    }
}

static int sys_write(const char *path, const char *value)
{
    check_sys_path(path);
    if (!safe_value(value))
        die("bad value");
    remember(path);
    /* Turning boost off makes amd-pstate lower scaling_max_freq by itself.
     * Record the max clocks now, after boost (so revert restores boost
     * first), or we would save the lowered value as the original. */
    size_t pl = strlen(path);
    if (pl > 14 && !strcmp(path + pl - 14, "/cpufreq/boost")) {
        glob_t g;
        if (!glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_max_freq", 0, NULL, &g)) {
            for (size_t i = 0; i < g.gl_pathc; i++)
                remember(g.gl_pathv[i]);
            globfree(&g);
        }
    }
    return write_file_value(path, value);
}

static int cmd_write(int argc, char **argv)
{
    if (argc != 2)
        die("usage: write <path> <value>");
    return sys_write(argv[0], argv[1]);
}

static int cmd_write_all(int argc, char **argv)
{
    if (argc != 2)
        die("usage: write-all <glob> <value>");
    if (has_dotdot(argv[0]))
        die("bad glob");
    glob_t g;
    if (glob(argv[0], GLOB_NOSORT, NULL, &g) || g.gl_pathc == 0)
        die("nothing matches %s", argv[0]);
    for (size_t i = 0; i < g.gl_pathc; i++)
        check_sys_path(g.gl_pathv[i]); /* all-or-nothing */
    int rc = 0;
    for (size_t i = 0; i < g.gl_pathc; i++)
        rc |= sys_write(g.gl_pathv[i], argv[1]);
    globfree(&g);
    return rc;
}

/* ---------------------------------------------------------------- sysctl */

static const char *const sysctl_prefix[] = {
    "vm.", "net.core.", "net.ipv4.tcp_", "net.ipv4.udp_", "net.ipv4.ip_local_port_range",
    "net.ipv6.conf.all.", "kernel.sched_", "kernel.perf_event_paranoid",
    "kernel.perf_event_max_sample_rate", "kernel.kptr_restrict", "kernel.nmi_watchdog",
    "kernel.split_lock_mitigate", "kernel.numa_balancing", "kernel.timer_migration",
    "fs.file-max", "fs.inotify.", NULL};
static const char *const sysctl_deny[] = {"net.core.bpf", "vm.mmap_min_addr",
                                          "vm.unprivileged_userfaultfd", NULL};

static int cmd_sysctl(int argc, char **argv)
{
    if (argc != 2)
        die("usage: sysctl <key> <value>");
    const char *key = argv[0];
    for (const char *c = key; *c; c++)
        if (!islower((unsigned char)*c) && !isdigit((unsigned char)*c) && !strchr("._-", *c))
            die("bad sysctl key");
    int ok = 0;
    for (int i = 0; sysctl_prefix[i]; i++)
        if (!strncmp(key, sysctl_prefix[i], strlen(sysctl_prefix[i])))
            ok = 1;
    for (int i = 0; sysctl_deny[i]; i++)
        if (!strncmp(key, sysctl_deny[i], strlen(sysctl_deny[i])))
            ok = 0;
    if (!ok || strstr(key, ".."))
        die("sysctl %s not allowed", key);
    if (!safe_value(argv[1]))
        die("bad value");
    char path[PATH_MAX] = "/proc/sys/";
    strncat(path, key, sizeof path - strlen(path) - 1);
    for (char *c = path + 10; *c; c++)
        if (*c == '.')
            *c = '/';
    char tag[300], buf[512];
    snprintf(tag, sizeof tag, "sysctl:%s", key);
    if (read_small(path, buf, sizeof buf) < 0)
        die("no such sysctl %s", key);
    buf[strcspn(buf, "\n")] = 0;
    for (char *c = buf; *c; c++)
        if (*c == '\t')
            *c = ' ';
    orig_save(tag, buf);
    return write_file_value(path, argv[1]);
}

/* -------------------------------------------------------- revert-runtime */

static int cmd_revert_runtime(void)
{
    FILE *f = fopen(ORIG_FILE, "r");
    if (!f) {
        puts("nothing to revert");
        return 0;
    }
    char line[1024];
    int rc = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        const char *key = line, *val = tab + 1;
        if (!strcmp(val, "@skip"))
            continue;
        if (!strncmp(key, "sysctl:", 7)) {
            char path[PATH_MAX] = "/proc/sys/";
            strncat(path, key + 7, sizeof path - strlen(path) - 1);
            for (char *c = path + 10; *c; c++)
                if (*c == '.')
                    *c = '/';
            rc |= write_file_value(path, val);
        } else if (!strcmp(val, "@od-reset")) {
            rc |= write_file_value(key, "r");
            rc |= write_file_value(key, "c");
        } else {
            rc |= write_file_value(key, val);
        }
    }
    fclose(f);
    if (!dry) {
        char done[PATH_MAX];
        snprintf(done, sizeof done, "%s.reverted-%s", ORIG_FILE, stamp);
        rename(ORIG_FILE, done);
    }
    return rc;
}

/* ------------------------------------------------------- file installs */

/* exact-ish destinations (fnmatch, FNM_PATHNAME) */
static const char *const dst_patterns[] = {
    "/etc/portage/make.conf",
    "/etc/portage/env/optimzr-*",
    "/etc/portage/package.env/optimzr-*",
    "/etc/portage/package.use/optimzr-*",
    "/etc/portage/package.accept_keywords/optimzr-*",
    "/etc/portage/package.mask/optimzr-*",
    "/etc/portage/package.unmask/optimzr-*",
    "/etc/portage/patches/*/*/optimzr-*.patch",
    "/etc/sysctl.d/90-optimzr*.conf",
    "/etc/modprobe.d/optimzr*.conf",
    "/etc/modules-load.d/optimzr*.conf",
    "/etc/udev/rules.d/[0-9][0-9]-optimzr*.rules",
    "/etc/local.d/optimzr*.start",
    "/etc/local.d/optimzr*.stop",
    "/etc/init.d/optimzr*",
    "/etc/conf.d/optimzr*",
    "/etc/dracut.conf.d/optimzr*.conf",
    "/etc/security/limits.d/optimzr*.conf",
    "/etc/default/grub",
    "/usr/local/bin/optimzr*",
    "/usr/local/sbin/optimzr*",
    "/usr/local/share/applications/optimzr*.desktop",
    "/usr/local/share/icons/hicolor/*/apps/optimzr*",
    NULL};
/* whole trees */
static const char *const dst_trees[] = {"/etc/optimzr/", "/usr/local/libexec/optimzr/",
                                        "/usr/local/share/optimzr/", NULL};

static void check_dst(const char *dst)
{
    if (dst[0] != '/' || has_dotdot(dst) || strstr(dst, "/.") )
        die("bad destination %s", dst);
    if (!strcmp(dst, SELF) || strstr(dst, "sudoers"))
        die("refusing to touch %s", dst);
    for (int i = 0; dst_patterns[i]; i++)
        if (!fnmatch(dst_patterns[i], dst, FNM_PATHNAME))
            return;
    for (int i = 0; dst_trees[i]; i++)
        if (!strncmp(dst, dst_trees[i], strlen(dst_trees[i])) && strlen(dst) > strlen(dst_trees[i]))
            return;
    die("not an allowed destination: %s", dst);
}

static int copy_fd_to(int in, const char *dst, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.optimzr-tmp", dst);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0)
        return -1;
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n) {
            close(out);
            unlink(tmp);
            return -1;
        }
    if (n < 0 || fchown(out, 0, 0) || fchmod(out, mode) || fsync(out)) {
        close(out);
        unlink(tmp);
        return -1;
    }
    close(out);
    return rename(tmp, dst);
}

static void backup(const char *path)
{
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode))
        return;
    char dst[PATH_MAX], dir[PATH_MAX];
    snprintf(dst, sizeof dst, "%s/%s%s", BACKUP_DIR, stamp, path);
    snprintf(dir, sizeof dir, "%s", dst);
    *strrchr(dir, '/') = 0;
    if (dry) {
        printf("would back up %s -> %s\n", path, dst);
        return;
    }
    mkdir_p(dir, 0700);
    int in = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0 || copy_fd_to(in, dst, st.st_mode & 07777))
        die("backup of %s failed: %s", path, strerror(errno));
    close(in);
    logmsg("backup %s -> %s", path, dst);
}

static int cmd_install(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
        die("usage: install <src> <dst> [0644|0755|0600]");
    const char *src = argv[0], *dst = argv[1];
    mode_t mode = 0644;
    if (argc == 3) {
        if (!strcmp(argv[2], "0755"))
            mode = 0755;
        else if (!strcmp(argv[2], "0600"))
            mode = 0600;
        else if (strcmp(argv[2], "0644"))
            die("mode must be 0644, 0755 or 0600");
    }
    check_dst(dst);
    int in = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (in < 0 || fstat(in, &st) || !S_ISREG(st.st_mode))
        die("source %s is not a regular file", src);
    if (st.st_size > 64L << 20)
        die("source too big");
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", dst);
    *strrchr(dir, '/') = 0;
    backup(dst);
    if (dry) {
        printf("would install %s -> %s (%o)\n", src, dst, mode);
        return 0;
    }
    struct stat ds;
    if (stat(dir, &ds)) {
        int tree = 0;
        for (int i = 0; dst_trees[i]; i++)
            if (!strncmp(dst, dst_trees[i], strlen(dst_trees[i])))
                tree = 1;
        if (!tree && strncmp(dir, "/etc/portage/", 13) && strncmp(dir, "/usr/local/share/icons/", 23))
            die("directory %s does not exist", dir);
        mkdir_p(dir, 0755);
    }
    if (copy_fd_to(in, dst, mode))
        die("install %s: %s", dst, strerror(errno));
    close(in);
    logmsg("install %s -> %s mode %o", src, dst, mode);
    return 0;
}

static int cmd_remove(int argc, char **argv)
{
    if (argc != 1)
        die("usage: remove <dst>");
    check_dst(argv[0]);
    if (!strcmp(argv[0], "/etc/default/grub") || !strcmp(argv[0], "/etc/portage/make.conf"))
        die("%s can be replaced or restored, not removed", argv[0]);
    backup(argv[0]);
    if (dry) {
        printf("would remove %s\n", argv[0]);
        return 0;
    }
    if (unlink(argv[0]))
        die("remove %s: %s", argv[0], strerror(errno));
    logmsg("remove %s", argv[0]);
    return 0;
}

static int list_cb(const char *p, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st;
    (void)ftw;
    if (type == FTW_F)
        puts(p + strlen(BACKUP_DIR) + 1);
    return 0;
}

static int cmd_backups(void)
{
    return nftw(BACKUP_DIR, list_cb, 16, FTW_PHYS) ? (puts("no backups"), 0) : 0;
}

static int cmd_restore(int argc, char **argv)
{
    if (argc != 2)
        die("usage: restore <stamp> <path>   (see `backups`)");
    if (!safe_word(argv[0]) || strchr(argv[0], '/'))
        die("bad stamp");
    check_dst(argv[1]);
    char src[PATH_MAX];
    snprintf(src, sizeof src, "%s/%s%s", BACKUP_DIR, argv[0], argv[1]);
    int in = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (in < 0 || fstat(in, &st))
        die("no backup %s", src);
    backup(argv[1]);
    if (dry) {
        printf("would restore %s -> %s\n", src, argv[1]);
        return 0;
    }
    if (copy_fd_to(in, argv[1], st.st_mode & 07777))
        die("restore failed: %s", strerror(errno));
    logmsg("restore %s -> %s", src, argv[1]);
    return 0;
}

/* --------------------------------------------------------------- emerge */

static const char *const atoms[] = {
    /* measuring */
    "sys-apps/dmidecode", "dev-util/perf", "sys-power/cpupower", "sys-apps/lm-sensors",
    "app-benchmarks/stress-ng", "app-benchmarks/sysbench", "sys-block/fio",
    "app-benchmarks/hyperfine", "sys-apps/smartmontools", "sys-apps/nvme-cli",
    "sys-apps/hwloc", "sys-apps/inxi", "sys-apps/hwinfo", "sys-process/numactl",
    "dev-util/vulkan-tools", "sys-apps/memtester", "sys-apps/pciutils", "sys-apps/usbutils",
    "dev-util/hip", "dev-util/rocminfo", "app-misc/radeontop",
    /* network */
    "net-misc/iperf", "net-analyzer/netperf", "net-analyzer/mtr", "net-analyzer/fping",
    "sys-apps/ethtool", "sys-apps/iproute2", "net-analyzer/tcpdump", "sys-apps/irqbalance",
    /* build speed */
    "dev-util/ccache", "sys-devel/mold", "dev-build/ninja", "dev-build/cmake",
    "app-arch/zstd", "app-arch/xz-utils", "app-arch/pigz", "app-arch/lz4", "app-arch/pbzip2",
    /* toolchain (rebuilds with pgo/lto) */
    "sys-devel/gcc", "llvm-core/llvm", "llvm-core/clang", "llvm-core/lld",
    "llvm-core/clang-runtime", "llvm-runtimes/compiler-rt", "sys-devel/binutils",
    "dev-lang/rust", "sys-libs/glibc",
    /* kernel */
    "dev-util/pahole", "sys-kernel/dracut", "sys-kernel/installkernel",
    "sys-kernel/linux-firmware", "sys-firmware/amd-microcode", "dev-util/bpftool",
    /* app */
    "gui-libs/gtk", "dev-libs/json-glib", "gui-libs/libadwaita", NULL};

static int cmd_emerge(int argc, char **argv)
{
    char *av[96];
    int n = 0, pretend = 0, oneshot = 0, natoms = 0;
    av[n++] = "emerge";
    av[n++] = "--ask=n";
    av[n++] = "--verbose";
    av[n++] = "--quiet-build=y";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--pretend"))
            pretend = 1;
        else if (!strcmp(argv[i], "-1") || !strcmp(argv[i], "--oneshot"))
            oneshot = 1;
        else if (in_list(argv[i], atoms))
            natoms++;
        else
            die("emerge: %s is not on the allow-list", argv[i]);
    }
    if (!natoms)
        die("usage: emerge [-p] [-1] <atom>...");
    if (pretend)
        av[n++] = "--pretend";
    if (oneshot)
        av[n++] = "--oneshot";
    for (int i = 0; i < argc && n < 94; i++)
        if (argv[i][0] != '-')
            av[n++] = argv[i];
    av[n] = NULL;
    return run(av);
}

static int cmd_world(const char *what, int argc, char **argv)
{
    int pretend = argc == 1 && !strcmp(argv[0], "-p");
    if (argc && !pretend)
        die("usage: %s [-p]", what);
    char *av[16];
    int n = 0;
    av[n++] = "emerge";
    av[n++] = "--ask=n";
    av[n++] = "--verbose";
    av[n++] = "--quiet-build=y";
    if (!strcmp(what, "world-update")) {
        av[n++] = "--update";
        av[n++] = "--deep";
        av[n++] = "--newuse";
        av[n++] = "--with-bdeps=y";
        av[n++] = "@world";
    } else if (!strcmp(what, "world-rebuild")) {
        av[n++] = "--emptytree";
        av[n++] = "@world";
    } else if (!strcmp(what, "preserved-rebuild")) {
        av[n++] = "@preserved-rebuild";
    } else if (!strcmp(what, "sync")) {
        n = 1;
        av[n++] = "--sync";
    }
    if (pretend)
        av[n++] = "--pretend";
    av[n] = NULL;
    return run(av);
}

/* ------------------------------------------------------------- services */

/* services the benchmarks may stop; extra ones come from services.local at build time */
#ifndef EXTRA_SERVICES
#define EXTRA_SERVICES
#endif
static const char *const services[] = {"cupsd", "bluetooth", EXTRA_SERVICES NULL};

static int cmd_svc(int argc, char **argv)
{
    static const char *const acts[] = {"start", "stop", "restart", "status", NULL};
    if (argc != 2 || !in_list(argv[1], acts))
        die("usage: svc <name> start|stop|restart|status");
    if (!in_list(argv[0], services) && strncmp(argv[0], "optimzr", 7))
        die("service %s not allowed", argv[0]);
    if (!safe_word(argv[0]) || strchr(argv[0], '/'))
        die("bad service name");
    char *av[] = {"rc-service", argv[0], argv[1], NULL};
    return run(av);
}

static int cmd_rc_update(int argc, char **argv)
{
    static const char *const acts[] = {"add", "del", NULL};
    static const char *const lvls[] = {"default", "boot", NULL};
    if (argc != 3 || !in_list(argv[0], acts) || !in_list(argv[2], lvls))
        die("usage: rc-update add|del optimzr-<svc> default|boot");
    if (strncmp(argv[1], "optimzr", 7) || !safe_word(argv[1]) || strchr(argv[1], '/'))
        die("only optimzr services");
    char *av[] = {"rc-update", argv[0], argv[1], argv[2], NULL};
    return run(av);
}

/* -------------------------------------------------------------- modules */

static int cmd_modprobe(int argc, char **argv)
{
    static const char *const mods[] = {"it87", "nct6775", "nct6683", "tcp_bbr", "sch_fq",
                                       "sch_cake", "sch_fq_pie", "msr", "cpuid", NULL};
    int rm = argc >= 1 && !strcmp(argv[0], "-r");
    if (rm) {
        argc--;
        argv++;
    }
    if (argc < 1 || !in_list(argv[0], mods))
        die("usage: modprobe [-r] <it87|nct6775|nct6683|tcp_bbr|sch_fq|sch_cake|sch_fq_pie|msr|cpuid> [param=value...]");
    char *av[16];
    int n = 0;
    av[n++] = "modprobe";
    if (rm)
        av[n++] = "-r";
    av[n++] = argv[0];
    for (int i = 1; i < argc && n < 15; i++) {
        if (rm || !safe_word(argv[i]) || !strchr(argv[i], '=') || argv[i][0] == '-')
            die("bad module parameter %s", argv[i]);
        av[n++] = argv[i];
    }
    av[n] = NULL;
    return run(av);
}

/* --------------------------------------------------------- ethtool / tc */

static void save_net_orig(const char *ifn)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/ethtool-orig-%s.txt", STATE_DIR, ifn);
    struct stat st;
    if (dry || !stat(path, &st) || !find_bin("ethtool"))
        return;
    mkdir_p(STATE_DIR, 0700);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    static const char *const flags[] = {"-k", "-g", "-c", "-l", "-a", "--show-eee", NULL};
    for (int i = 0; flags[i]; i++) {
        dprintf(fd, "### ethtool %s %s\n", flags[i], ifn);
        pid_t pid = fork();
        if (pid == 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            char *av[] = {"ethtool", (char *)flags[i], (char *)ifn, NULL};
            execve(find_bin("ethtool"), av, clean_env);
            _exit(127);
        }
        waitpid(pid, NULL, 0);
    }
    close(fd);
    logmsg("saved NIC settings of %s to %s", ifn, path);
}

static int cmd_ethtool(int argc, char **argv)
{
    static const char *const rd[] = {"-i", "-k", "-g", "-c", "-l", "-S", "-a", "-m",
                                     "--show-eee", "--show-features", "-T", NULL};
    static const char *const wr[] = {"-K", "-G", "-C", "-L", "-A", "--set-eee", NULL};
    if (argc < 2)
        die("usage: ethtool <flag> <iface> [args...]");
    if (!wired_nic(argv[1]))
        die("%s is not a wired NIC", argv[1]);
    int write = in_list(argv[0], wr);
    if (!write && !in_list(argv[0], rd))
        die("ethtool flag %s not allowed", argv[0]);
    if (!write && argc != 2)
        die("read flags take no extra args");
    char *av[48];
    int n = 0;
    av[n++] = "ethtool";
    av[n++] = argv[0];
    av[n++] = argv[1];
    for (int i = 2; i < argc && n < 47; i++) {
        if (!safe_word(argv[i]) || argv[i][0] == '-')
            die("bad ethtool arg %s", argv[i]);
        av[n++] = argv[i];
    }
    av[n] = NULL;
    if (write)
        save_net_orig(argv[1]);
    return run(av);
}

static int cmd_tc(int argc, char **argv)
{
    static const char *const kinds[] = {"fq", "fq_codel", "cake", "fq_pie", "pfifo_fast", NULL};
    if (argc >= 1 && !strcmp(argv[0], "show")) {
        char *av[] = {"tc", "-s", "qdisc", "show", NULL};
        return run(av);
    }
    /* replace <iface> <kind> [params...] */
    if (argc < 3 || strcmp(argv[0], "replace") || !wired_nic(argv[1]) || !in_list(argv[2], kinds))
        die("usage: tc show | tc replace <iface> <fq|fq_codel|cake|fq_pie|pfifo_fast> [params]");
    char *av[48];
    int n = 0;
    av[n++] = "tc";
    av[n++] = "qdisc";
    av[n++] = "replace";
    av[n++] = "dev";
    av[n++] = argv[1];
    av[n++] = "root";
    av[n++] = argv[2];
    for (int i = 3; i < argc && n < 47; i++) {
        if (!safe_word(argv[i]) || argv[i][0] == '-')
            die("bad tc arg %s", argv[i]);
        av[n++] = argv[i];
    }
    av[n] = NULL;
    if (!orig_known("tc"))
        orig_save("tc", "@skip"); /* note only; revert = `tc replace <if> fq_codel` */
    return run(av);
}

/* ------------------------------------------------------- read-only info */

static int cmd_info(void)
{
    char *dmi[] = {"dmidecode", "-t", "bios", "-t", "baseboard", "-t", "processor",
                   "-t", "memory", NULL};
    run(dmi);
    glob_t g;
    if (!glob("/dev/nvme[0-9]", 0, NULL, &g)) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char *sm[] = {"smartctl", "-a", g.gl_pathv[i], NULL};
            run(sm);
        }
        globfree(&g);
    }
    return 0;
}

static int cmd_dmesg(void)
{
    char *av[] = {"dmesg", "--ctime", NULL};
    return run(av);
}

static int cmd_rapl(void)
{
    glob_t g;
    if (glob("/sys/class/powercap/intel-rapl:*", 0, NULL, &g))
        die("no RAPL");
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("t=%ld.%09ld\n", (long)ts.tv_sec, ts.tv_nsec);
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char p[PATH_MAX], name[64], e[64];
        snprintf(p, sizeof p, "%s/name", g.gl_pathv[i]);
        if (read_small(p, name, sizeof name) < 0)
            continue;
        snprintf(p, sizeof p, "%s/energy_uj", g.gl_pathv[i]);
        if (read_small(p, e, sizeof e) < 0)
            continue;
        name[strcspn(name, "\n")] = 0;
        e[strcspn(e, "\n")] = 0;
        printf("%s %s %s\n", strrchr(g.gl_pathv[i], '/') + 1, name, e);
    }
    globfree(&g);
    return 0;
}

static int cmd_turbostat(int argc, char **argv)
{
    long secs = argc == 1 ? strtol(argv[0], NULL, 10) : 0;
    if (secs < 1 || secs > 3600)
        die("usage: turbostat <seconds 1-3600>");
    char nb[16];
    snprintf(nb, sizeof nb, "%ld", secs);
    const char *bin = find_bin("optimzr-turbostat") ? "optimzr-turbostat" : "turbostat";
    char *av[] = {(char *)bin, "--quiet", "--interval", "1", "--num_iterations", nb, NULL};
    return run(av);
}

/* --------------------------------------------------------------- kernel */

static int release_ok(const char *rel)
{
    if (!safe_word(rel) || strchr(rel, '/') || !strstr(rel, "gentoo-power"))
        return 0;
    struct utsname u;
    uname(&u);
    return strcmp(rel, u.release) != 0;
}

static int cmd_kernel_install(int argc, char **argv)
{
    if (argc != 1)
        die("usage: kernel-install <build dir under " PROJECT_DIR "/kernel/>");
    char real[PATH_MAX];
    if (!realpath(argv[0], real))
        die("%s: %s", argv[0], strerror(errno));
    const char *base = PROJECT_DIR "/kernel/";
    if (strncmp(real, base, strlen(base)))
        die("build dir must be under %s", base);
    char p[PATH_MAX], rel[128], cfg[256];
    snprintf(p, sizeof p, "%s/include/config/kernel.release", real);
    if (read_small(p, rel, sizeof rel) < 0)
        die("no kernel.release, build the kernel first");
    rel[strcspn(rel, "\n")] = 0;
    if (!release_ok(rel))
        die("release %s must contain gentoo-power and not be the running kernel", rel);
    snprintf(p, sizeof p, "%s/arch/x86/boot/bzImage", real);
    struct stat st;
    if (stat(p, &st))
        die("no bzImage");
    /* clang-built kernels need LLVM= or make wants to reconfigure; give it the
     * LLVM bin dir because our clean PATH does not have llvm-strip etc. */
    int llvm = 0, clangver = 0;
    static char llvmarg[96] = "V=0";
    snprintf(p, sizeof p, "%s/.config", real);
    FILE *f = fopen(p, "r");
    if (f) {
        while (fgets(cfg, sizeof cfg, f)) {
            if (!strncmp(cfg, "CONFIG_CC_IS_CLANG=y", 20))
                llvm = 1;
            sscanf(cfg, "CONFIG_CLANG_VERSION=%d", &clangver);
        }
        fclose(f);
    }
    if (llvm) {
        char dir[64];
        snprintf(dir, sizeof dir, "/usr/lib/llvm/%d/bin/", clangver / 10000);
        if (clangver >= 10000 && !access(dir, X_OK))
            snprintf(llvmarg, sizeof llvmarg, "LLVM=%s", dir);
        else
            snprintf(llvmarg, sizeof llvmarg, "LLVM=1");
    }
    char *mi[] = {"make", "-C", real, "INSTALL_MOD_STRIP=1", llvmarg, "modules_install", NULL};
    char *in[] = {"make", "-C", real, llvmarg, "install", NULL};
    logmsg("kernel-install %s from %s", rel, real);
    int rc = run(mi);
    if (!rc)
        rc = run(in);
    /* make as root leaves root-owned files in the user's build tree */
    const char *su = getenv("SUDO_UID"), *sg = getenv("SUDO_GID");
    if (su && sg && safe_word(su) && safe_word(sg)) {
        char from[] = "--from=0:0", own[64];
        snprintf(own, sizeof own, "%s:%s", su, sg);
        char *ch[] = {"chown", "-R", "--no-dereference", from, own, real, NULL};
        run(ch);
    }
    return rc;
}

static int cmd_kernel_remove(int argc, char **argv)
{
    if (argc != 1 || !release_ok(argv[0]))
        die("usage: kernel-remove <release containing gentoo-power, not the running one>");
    const char *rel = argv[0];
    static const char *const fmt[] = {"/boot/vmlinuz-%s", "/boot/System.map-%s", "/boot/config-%s",
                                      "/boot/initramfs-%s.img", NULL};
    for (int i = 0; fmt[i]; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, fmt[i], rel);
        if (dry)
            printf("would remove %s\n", p);
        else if (!unlink(p))
            logmsg("remove %s", p);
    }
    char mod[PATH_MAX];
    snprintf(mod, sizeof mod, "/lib/modules/%s", rel);
    char *rm[] = {"rm", "-rf", "--one-file-system", mod, NULL};
    run(rm);
    char *gm[] = {"grub-mkconfig", "-o", "/boot/grub/grub.cfg", NULL};
    return run(gm);
}

static int cmd_grub_mkconfig(void)
{
    char *gm[] = {"grub-mkconfig", "-o", "/boot/grub/grub.cfg", NULL};
    return run(gm);
}

/* read-only: the menu entries and their ids, to check GRUB_DEFAULT against */
static int cmd_grub_entries(void)
{
    FILE *f = fopen("/boot/grub/grub.cfg", "r");
    if (!f)
        die("/boot/grub/grub.cfg: %s", strerror(errno));
    char line[1024];
    int depth = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line + strspn(line, " \t");
        int sub = !strncmp(s, "submenu ", 8);
        if (sub || !strncmp(s, "menuentry ", 10)) {
            char title[256] = "", id[256] = "";
            sscanf(s, "%*s '%255[^']'", title);
            char *m = strstr(s, "menuentry_id_option '");
            if (m)
                sscanf(m + 21, "%255[^']", id);
            printf("%s%s%s\n    id %s\n", depth ? "  > " : "", sub ? "[submenu] " : "", title, id);
            if (sub)
                depth = 1;
        }
    }
    fclose(f);
    return 0;
}

/* ----------------------------------------------------------------- main */

static void usage(void)
{
    puts("optimzr-root [-n] <command> ...   (-n = dry run)\n"
         "  status | info | dmesg | rapl | turbostat <secs>\n"
         "  write <path> <value> | write-all <glob> <value> | sysctl <key> <value>\n"
         "  revert-runtime\n"
         "  install <src> <dst> [mode] | remove <dst> | backups | restore <stamp> <dst>\n"
         "  emerge [-p] [-1] <atom>... | world-update [-p] | world-rebuild [-p]\n"
         "  preserved-rebuild | sync\n"
         "  svc <name> start|stop|restart|status | rc-update add|del optimzr-* <level>\n"
         "  modprobe [-r] <module> [p=v...]\n"
         "  ethtool <flag> <iface> [args] | tc show | tc replace <iface> <qdisc> [params]\n"
         "  kernel-install <builddir> | kernel-remove <release> | grub-mkconfig | grub-entries");
}

int main(int argc, char **argv)
{
    umask(022);
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(stamp + strlen(stamp), sizeof stamp - strlen(stamp), "-%d", (int)getpid());

    argc--;
    argv++;
    if (argc && !strcmp(argv[0], "-n")) {
        dry = 1;
        argc--;
        argv++;
    }
    if (!argc) {
        usage();
        return 2;
    }
    const char *cmd = argv[0];
    argc--;
    argv++;

    if (!strcmp(cmd, "status")) {
        printf("optimzr-root for %s\nstate %s, log %s\n", PROJECT_DIR, STATE_DIR, LOG_FILE);
        printf("euid %d\n", (int)geteuid());
        return 0;
    }
    if (geteuid() != 0 && !dry)
        die("must run as root (sudo optimzr-root ...)");
    if (!dry) {
        char line[1024] = "";
        strncat(line, cmd, sizeof line - 1);
        for (int i = 0; i < argc; i++) {
            strncat(line, " ", sizeof line - strlen(line) - 1);
            strncat(line, argv[i], sizeof line - strlen(line) - 1);
        }
        logmsg("cmd %s", line);
    }

    if (!strcmp(cmd, "info")) return cmd_info();
    if (!strcmp(cmd, "dmesg")) return cmd_dmesg();
    if (!strcmp(cmd, "rapl")) return cmd_rapl();
    if (!strcmp(cmd, "turbostat")) return cmd_turbostat(argc, argv);
    if (!strcmp(cmd, "write")) return cmd_write(argc, argv);
    if (!strcmp(cmd, "write-all")) return cmd_write_all(argc, argv);
    if (!strcmp(cmd, "sysctl")) return cmd_sysctl(argc, argv);
    if (!strcmp(cmd, "revert-runtime")) return cmd_revert_runtime();
    if (!strcmp(cmd, "install")) return cmd_install(argc, argv);
    if (!strcmp(cmd, "remove")) return cmd_remove(argc, argv);
    if (!strcmp(cmd, "backups")) return cmd_backups();
    if (!strcmp(cmd, "restore")) return cmd_restore(argc, argv);
    if (!strcmp(cmd, "emerge")) return cmd_emerge(argc, argv);
    if (!strcmp(cmd, "world-update") || !strcmp(cmd, "world-rebuild") ||
        !strcmp(cmd, "preserved-rebuild") || !strcmp(cmd, "sync"))
        return cmd_world(cmd, argc, argv);
    if (!strcmp(cmd, "svc")) return cmd_svc(argc, argv);
    if (!strcmp(cmd, "rc-update")) return cmd_rc_update(argc, argv);
    if (!strcmp(cmd, "modprobe")) return cmd_modprobe(argc, argv);
    if (!strcmp(cmd, "ethtool")) return cmd_ethtool(argc, argv);
    if (!strcmp(cmd, "tc")) return cmd_tc(argc, argv);
    if (!strcmp(cmd, "kernel-install")) return cmd_kernel_install(argc, argv);
    if (!strcmp(cmd, "kernel-remove")) return cmd_kernel_remove(argc, argv);
    if (!strcmp(cmd, "grub-mkconfig")) return cmd_grub_mkconfig();
    if (!strcmp(cmd, "grub-entries")) return cmd_grub_entries();
    usage();
    return 2;
}
