/*
 * probe.c - Core validation helper for mini-tmux harness.
 *
 * Usage: probe <sideband_socket_path> <session_id>
 *
 * Runs inside a mini-tmux pane. Reports environment checks, signal reception,
 * and IO token exchange through the sideband channel (Unix domain socket).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>

#include "sideband_common.h"

static const char *g_session = NULL;

static int sig_pipe[2] = {-1, -1};

static void sig_handler(int signo) {
    unsigned char s = (unsigned char)signo;
    (void)write(sig_pipe[1], &s, 1);
}

static const char *signame(int signo) {
    switch (signo) {
        case SIGINT:    return "SIGINT";
        case SIGTSTP:   return "SIGTSTP";
        case SIGWINCH:  return "SIGWINCH";
        case SIGCONT:   return "SIGCONT";
        default:        return "UNKNOWN";
    }
}

static void send_env_check(void) {
    int isatty_in  = isatty(STDIN_FILENO);
    int isatty_out = isatty(STDOUT_FILENO);
    struct winsize ws = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    pid_t pid   = getpid();
    pid_t pgrp  = getpgrp();
    pid_t tcpgrp = -1;
    if (isatty_in) {
        tcpgrp = tcgetpgrp(STDIN_FILENO);
    }
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"env_check\",\"session\":\"%s\","
        "\"isatty_stdin\":%s,\"isatty_stdout\":%s,"
        "\"winsize\":{\"rows\":%d,\"cols\":%d},"
        "\"pid\":%d,\"pgrp\":%d,\"tcpgrp\":%d}",
        g_session,
        isatty_in  ? "true" : "false",
        isatty_out ? "true" : "false",
        ws.ws_row, ws.ws_col,
        (int)pid, (int)pgrp, (int)tcpgrp);
    sb_send(buf);
}

static void send_signal_report(int signo) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"signal\",\"session\":\"%s\","
        "\"signal\":\"%s\",\"pid\":%d,\"pgrp\":%d}",
        g_session, signame(signo), (int)getpid(), (int)getpgrp());
    sb_send(buf);

    if (signo == SIGWINCH) {
        send_env_check();
    }
}

static void generate_hex_token(char *buf, size_t len) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len - 1; i++) {
        buf[i] = hex[rand() % 16];
    }
    buf[len - 1] = '\0';
}

static void send_output_token(void) {
    char token[33];
    generate_hex_token(token, sizeof(token));

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"output_token\",\"session\":\"%s\",\"token\":\"%s\"}",
        g_session, token);
    sb_send(buf);

    printf("%s\n", token);
    fflush(stdout);
}

static int is_hex_token(const char *s, size_t len) {
    if (len != 32) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static void handle_input_token(const char *line) {
    size_t len = strlen(line);
    if (!is_hex_token(line, len)) return;

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"input_token\",\"session\":\"%s\",\"received\":\"%s\"}",
        g_session, line);
    sb_send(buf);
}

static void send_ready(void) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"ready\",\"session\":\"%s\",\"pid\":%d}",
        g_session, (int)getpid());
    sb_send(buf);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: probe <sideband_socket> <session_id>\n");
        return 1;
    }
    const char *sock_path = argv[1];
    g_session = argv[2];

    srand((unsigned)(time(NULL) ^ getpid()));

    setvbuf(stdout, NULL, _IONBF, 0);

    if (sb_connect(sock_path) < 0) {
        fprintf(stderr, "probe: cannot connect to sideband at %s\n", sock_path);
        return 1;
    }

    if (pipe(sig_pipe) < 0) {
        perror("pipe");
        return 1;
    }
    fcntl(sig_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(sig_pipe[1], F_SETFL, O_NONBLOCK);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,    &sa, NULL);
    sigaction(SIGTSTP,   &sa, NULL);
    sigaction(SIGWINCH,  &sa, NULL);
    sigaction(SIGCONT,   &sa, NULL);

    send_env_check();
    send_ready();
    send_output_token();

    fd_set rfds;
    char line_buf[4096];
    size_t line_pos = 0;

    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sig_pipe[0], &rfds);
        int maxfd = (STDIN_FILENO > sig_pipe[0]) ? STDIN_FILENO : sig_pipe[0];

        struct timeval tv = {1, 0};
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && FD_ISSET(sig_pipe[0], &rfds)) {
            unsigned char s;
            while (read(sig_pipe[0], &s, 1) == 1) {
                send_signal_report((int)s);
            }
        }

        if (ret > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n <= 0) break;
            if (c == '\n' || c == '\r') {
                line_buf[line_pos] = '\0';
                handle_input_token(line_buf);
                line_pos = 0;
            } else if (line_pos < sizeof(line_buf) - 1) {
                line_buf[line_pos++] = c;
            }
        }

        if (ret == 0) {
            send_output_token();
        }
    }

    sb_close();
    return 0;
}
