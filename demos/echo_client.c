// echo_client.c — 配合 echo_server 使用
//
// 编译：gcc -o echo_client echo_client.c
// 运行：先启动 echo_server，再运行 ./echo_client

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/tmp/mini-tmux-demo.sock"

int main(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }
    printf("[client] 已连接\n");

    const char *msg = "hello from client";
    write(fd, msg, strlen(msg));
    printf("[client] 发送: %s\n", msg);

    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    printf("[client] 收到回复: %s\n", buf);

    close(fd);
    return 0;
}
