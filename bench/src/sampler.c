/* sampler: logs CPU clocks/temp, GPU temp/power/busy and free RAM while a
 * benchmark runs. Stops on SIGTERM/SIGINT and prints a one-line summary.
 *
 *   sampler <out.csv> [interval_ms]
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static long read_long(const char *p)
{
    FILE *f = fopen(p, "r");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

/* first hwmon dir whose name matches */
static int find_hwmon(const char *name, char *out, size_t sz)
{
    glob_t g;
    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &g))
        return -1;
    int found = -1;
    for (size_t i = 0; i < g.gl_pathc && found; i++) {
        char buf[64] = "";
        FILE *f = fopen(g.gl_pathv[i], "r");
        if (!f)
            continue;
        if (fgets(buf, sizeof buf, f)) {
            buf[strcspn(buf, "\n")] = 0;
            if (!strcmp(buf, name)) {
                snprintf(out, sz, "%.*s", (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]),
                         g.gl_pathv[i]);
                found = 0;
            }
        }
        fclose(f);
    }
    globfree(&g);
    return found;
}

static long mem_avail_mb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long kb = -1;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1)
            break;
    if (f)
        fclose(f);
    return kb / 1024;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sampler <out.csv> [interval_ms]\n");
        return 2;
    }
    int ms = argc > 2 ? atoi(argv[2]) : 500;
    if (ms < 50)
        ms = 50;
    FILE *out = fopen(argv[1], "w");
    if (!out) {
        perror(argv[1]);
        return 1;
    }
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    char k10[256] = "", gpu[256] = "", p[300];
    find_hwmon("k10temp", k10, sizeof k10);
    find_hwmon("amdgpu", gpu, sizeof gpu);
    int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);

    fprintf(out, "t_s,cpu_avg_mhz,cpu_max_mhz,tctl_c,gpu_edge_c,gpu_junc_c,gpu_w,gpu_busy,mem_avail_mb\n");
    double sum_avg = 0, sum_t = 0, sum_gw = 0, max_t = 0, max_gj = 0, max_mhz = 0;
    long n = 0, min_mem = 1L << 40;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!stop) {
        long s = 0, mx = 0, cnt = 0;
        for (int c = 0; c < ncpu; c++) {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
            long f = read_long(p);
            if (f > 0) {
                s += f;
                cnt++;
                if (f > mx)
                    mx = f;
            }
        }
        double avg = cnt ? s / (double)cnt / 1000.0 : 0;
        snprintf(p, sizeof p, "%s/temp1_input", k10);
        double tctl = read_long(p) / 1000.0;
        snprintf(p, sizeof p, "%s/temp1_input", gpu);
        double ge = read_long(p) / 1000.0;
        snprintf(p, sizeof p, "%s/temp2_input", gpu);
        double gj = read_long(p) / 1000.0;
        snprintf(p, sizeof p, "%s/power1_average", gpu);
        double gw = read_long(p) / 1e6;
        long busy = read_long("/sys/class/drm/card1/device/gpu_busy_percent");
        long mem = mem_avail_mb();
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(out, "%.2f,%.0f,%.0f,%.1f,%.1f,%.1f,%.1f,%ld,%ld\n", t, avg, mx / 1000.0, tctl, ge,
                gj, gw, busy, mem);
        fflush(out);
        n++;
        sum_avg += avg;
        sum_t += tctl;
        sum_gw += gw;
        if (tctl > max_t)
            max_t = tctl;
        if (gj > max_gj)
            max_gj = gj;
        if (mx / 1000.0 > max_mhz)
            max_mhz = mx / 1000.0;
        if (mem >= 0 && mem < min_mem)
            min_mem = mem;
        struct timespec d = {ms / 1000, (ms % 1000) * 1000000L};
        nanosleep(&d, NULL);
    }
    fclose(out);
    if (n)
        printf("samples=%ld cpu_avg_mhz=%.0f cpu_peak_mhz=%.0f tctl_avg=%.1f tctl_max=%.1f "
               "gpu_junc_max=%.1f gpu_w_avg=%.1f mem_avail_min_mb=%ld\n",
               n, sum_avg / n, max_mhz, sum_t / n, max_t, max_gj, sum_gw / n, min_mem);
    return 0;
}
