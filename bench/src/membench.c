/* membench: memory latency (random pointer chase) and bandwidth
 * (read / write / copy, all threads). Prints "key value unit" lines.
 *
 *   membench [threads]
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void *big_alloc(size_t sz)
{
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    madvise(p, sz, MADV_HUGEPAGE);
    memset(p, 1, sz);
    return p;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t xr(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* one pointer per 64-byte line, visited in a random cycle */
static double latency_ns(size_t bytes)
{
    size_t n = bytes / 64;
    char *buf = big_alloc(bytes);
    size_t *order = malloc(n * sizeof *order);
    for (size_t i = 0; i < n; i++)
        order[i] = i;
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = xr() % (i + 1), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (size_t i = 0; i < n; i++)
        *(void **)(buf + order[i] * 64) = buf + order[(i + 1) % n] * 64;
    free(order);

    void **p = (void **)buf;
    long iters = 1L << 24;
    for (long i = 0; i < iters / 4; i++) /* warm */
        p = *p;
    double best = 1e9;
    for (int r = 0; r < 3; r++) {
        double t = now();
        for (long i = 0; i < iters; i++)
            p = *p;
        double ns = (now() - t) * 1e9 / iters;
        if (ns < best)
            best = ns;
    }
    if (!p)
        puts("");
    munmap(buf, bytes);
    return best;
}

enum { OP_READ, OP_WRITE, OP_COPY };
struct job {
    char *a, *b;
    size_t len;
    int op;
    pthread_barrier_t *bar;
    uint64_t sink;
};

static void *worker(void *arg)
{
    struct job *j = arg;
    pthread_barrier_wait(j->bar);
    if (j->op == OP_READ) {
        const uint64_t *s = (const uint64_t *)j->a;
        uint64_t acc[8] = {0};
        for (size_t i = 0; i < j->len / 8; i += 8)
            for (int k = 0; k < 8; k++)
                acc[k] += s[i + k];
        for (int k = 0; k < 8; k++)
            j->sink += acc[k];
    } else if (j->op == OP_WRITE) {
        memset(j->a, (int)j->len, j->len);
    } else {
        memcpy(j->b, j->a, j->len);
    }
    pthread_barrier_wait(j->bar);
    return NULL;
}

static double bandwidth_gbs(int threads, int op, char *a, char *b, size_t total)
{
    pthread_t th[256];
    struct job jobs[256];
    pthread_barrier_t bar;
    double best = 0;
    size_t per = total / threads & ~(size_t)4095;
    for (int r = 0; r < 5; r++) {
        pthread_barrier_init(&bar, NULL, threads + 1);
        for (int i = 0; i < threads; i++) {
            jobs[i] = (struct job){a + i * per, b + i * per, per, op, &bar, 0};
            pthread_create(&th[i], NULL, worker, &jobs[i]);
        }
        pthread_barrier_wait(&bar);
        double t = now();
        pthread_barrier_wait(&bar);
        t = now() - t;
        for (int i = 0; i < threads; i++)
            pthread_join(th[i], NULL);
        pthread_barrier_destroy(&bar);
        /* copy moves the bytes twice (read + write) */
        double gbs = (double)per * threads * (op == OP_COPY ? 2 : 1) / t / 1e9;
        if (gbs > best)
            best = gbs;
    }
    return best;
}

int main(int argc, char **argv)
{
    int threads = argc > 1 ? atoi(argv[1]) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (threads < 1 || threads > 256)
        threads = 16;

    static const size_t sizes[] = {16 << 10, 32 << 10, 256 << 10, 1 << 20, 4 << 20, 16 << 20,
                                   28 << 20, 64 << 20, 256 << 20, 1024 << 20};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++)
        printf("lat_%zuK %.2f ns\n", sizes[i] >> 10, latency_ns(sizes[i]));

    size_t total = (size_t)2 << 30;
    char *a = big_alloc(total), *b = big_alloc(total);
    printf("bw_read %.1f GB/s\n", bandwidth_gbs(threads, OP_READ, a, b, total));
    printf("bw_write %.1f GB/s\n", bandwidth_gbs(threads, OP_WRITE, a, b, total));
    printf("bw_copy %.1f GB/s\n", bandwidth_gbs(threads, OP_COPY, a, b, total));
    printf("bw_read_1t %.1f GB/s\n", bandwidth_gbs(1, OP_READ, a, b, total / 4));
    return 0;
}
