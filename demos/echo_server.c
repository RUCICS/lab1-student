// echo_server.c — 最小的 Unix domain socket server
//
// 编译：gcc -o echo_server echo_server.c
// 运行：先启动 server，再在另一个终端启动 client
//
//   终端1: ./echo_server
//   终端2: ./echo_client
//
// server 收到 client 发来的消息，原样回复，然后退出。
// 这就是 mini-tmux 的 Client-Server 通信骨架。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/tmp/mini-tmux-demo.sock"

int main(void) {
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCK_PATH);  // 清理上次残留的 socket 文件
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(listen_fd, 1);

    printf("[server] 等待 client 连接...\n");
    int conn_fd = accept(listen_fd, NULL, NULL);
    printf("[server] client 已连接\n");

    char buf[256];
    int n = read(conn_fd, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    printf("[server] 收到: %s\n", buf);

    write(conn_fd, buf, n);  // 原样回复
    printf("[server] 已回复，退出\n");

    close(conn_fd);
    close(listen_fd);
    unlink(SOCK_PATH);
    return 0;
}
