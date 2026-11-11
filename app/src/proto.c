#define _GNU_SOURCE
#include "proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int proto_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    const char *path = getenv("OPTIMZR_SOCK");
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path && *path ? path : PROTO_SOCK);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int proto_call(int fd, const char *req, struct strbuf *out)
{
    char line[PROTO_MAX_LINE];
    int n = snprintf(line, sizeof line, "%s\n", req);
    out->len = 0;
    out->s[0] = 0;
    if (n <= 0 || (size_t)n >= sizeof line || write(fd, line, (size_t)n) != n)
        return -1;
    for (;;) {
        if (out->len >= sizeof out->s - 1)
            return -1;
        ssize_t r = read(fd, out->s + out->len, sizeof out->s - 1 - out->len);
        if (r <= 0)
            return -1;
        out->len += (size_t)r;
        out->s[out->len] = 0;
        size_t L = out->len;
        if ((L >= 2 && !strcmp(out->s + L - 2, ".\n") && (L == 2 || out->s[L - 3] == '\n')))
            break;
    }
    out->len -= 2;
    out->s[out->len] = 0;
    return strncmp(out->s, "ok", 2) ? 1 : 0;
}

const char *proto_get(const struct strbuf *ans, const char *key, char *buf, size_t sz)
{
    size_t kl = strlen(key);
    const char *p = ans->s;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '\n');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= sz)
                n = sz - 1;
            memcpy(buf, v, n);
            buf[n] = 0;
            return buf;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return NULL;
}
