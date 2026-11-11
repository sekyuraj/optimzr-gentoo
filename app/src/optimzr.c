/* optimzr: command line client for optimzrd.
 *
 *   optimzr                       status
 *   optimzr 50|60|70|80|90|100    pick a power level
 *   optimzr net <auto|balanced|lowlatency|throughput>
 *   optimzr auto on|off
 *   optimzr caps | monitor | stock
 *   optimzr watch                 monitor every second
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "proto.h"

static int call(int fd, const char *req)
{
    struct strbuf ans;
    int r = proto_call(fd, req, &ans);
    if (r < 0) {
        fprintf(stderr, "optimzr: lost connection to optimzrd\n");
        return 1;
    }
    if (r) {
        fprintf(stderr, "optimzr: %s\n", ans.s);
        return 1;
    }
    const char *body = strchr(ans.s, '\n');
    if (body)
        fputs(body + 1, stdout);
    return 0;
}

int main(int argc, char **argv)
{
    int fd = proto_connect();
    if (fd < 0) {
        perror("optimzr: connect " PROTO_SOCK);
        fprintf(stderr, "is optimzrd running, and are you in its group?\n");
        return 1;
    }
    char req[PROTO_MAX_LINE];
    if (argc == 1)
        return call(fd, "status");
    if (argc == 2 && !strcmp(argv[1], "watch")) {
        for (;;) {
            fputs("\033[H\033[2J", stdout);
            if (call(fd, "monitor"))
                return 1;
            fflush(stdout);
            sleep(1);
        }
    }
    if (argc == 2 && (!strcmp(argv[1], "caps") || !strcmp(argv[1], "monitor") ||
                      !strcmp(argv[1], "stock") || !strcmp(argv[1], "status")))
        return call(fd, argv[1]);
    if (argc == 2 && atoi(argv[1]) > 0) {
        snprintf(req, sizeof req, "profile %d", atoi(argv[1]));
        return call(fd, req);
    }
    if (argc == 3 && (!strcmp(argv[1], "net") || !strcmp(argv[1], "auto") ||
                      !strcmp(argv[1], "profile"))) {
        if (strlen(argv[2]) > 20)
            return 2;
        snprintf(req, sizeof req, "%s %s", argv[1], argv[2]);
        return call(fd, req);
    }
    fprintf(stderr, "usage: optimzr [50..100 | net <auto|balanced|lowlatency|throughput> |\n"
                    "                auto on|off | caps | monitor | watch | stock]\n");
    return 2;
}
