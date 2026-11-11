/* optimzrd protocol: one request per line, answer is "ok" or "err <why>",
 * then key=value lines, then a line with a single ".".
 *
 *   status                 profile, active level, net, auto, workload, levels
 *   caps                   what this machine supports
 *   monitor                one sensor snapshot (updated every second)
 *   profile <50..100>      pick a level
 *   net <auto|balanced|lowlatency|throughput>
 *   auto <on|off>          let games/builds raise the level
 *   stock                  undo everything optimzrd ever changed
 */
#ifndef PROTO_H
#define PROTO_H

#include <stddef.h>

#define PROTO_DIR "/run/optimzr"
#define PROTO_SOCK PROTO_DIR "/optimzrd.sock"
#define PROTO_MAX_LINE 256

struct strbuf {
    char s[16384];
    size_t len;
};

/* client side (proto.c) */
int proto_connect(void);
/* send one request, read the whole answer into out; 0 = "ok" */
int proto_call(int fd, const char *req, struct strbuf *out);
/* value of key in an answer, NULL if missing; copies into buf */
const char *proto_get(const struct strbuf *ans, const char *key, char *buf, size_t sz);

#endif
