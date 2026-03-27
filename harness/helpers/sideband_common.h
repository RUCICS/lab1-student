/*
 * sideband_common.h - Shared sideband communication code for probe helpers.
 *
 * Connects to a Unix domain socket and sends newline-delimited JSON messages.
 * Header-only for simplicity: include in each helper .c file.
 */
#ifndef SIDEBAND_COMMON_H
#define SIDEBAND_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

static int sb_fd = -1;

static int sb_connect(const char *socket_path) {
    struct sockaddr_un addr;
    sb_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sb_fd < 0) {
        perror("sb_connect: socket");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    if (connect(sb_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("sb_connect: connect");
        close(sb_fd);
        sb_fd = -1;
        return -1;
    }
    return 0;
}

static int sb_send(const char *json_msg) {
    if (sb_fd < 0) return -1;
    size_t len = strlen(json_msg);
    char buf[4096];
    if (len + 1 >= sizeof(buf)) return -1;
    memcpy(buf, json_msg, len);
    buf[len] = '\n';
    ssize_t n = write(sb_fd, buf, len + 1);
    return (n == (ssize_t)(len + 1)) ? 0 : -1;
}

static void sb_close(void) {
    if (sb_fd >= 0) {
        close(sb_fd);
        sb_fd = -1;
    }
}

#endif /* SIDEBAND_COMMON_H */
