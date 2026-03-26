// pty_shell.c — 用 50 行代码造一个"终端"
//
// 编译：gcc -o pty_shell pty_shell.c -lutil
// 运行：./pty_shell
//
// 你会得到一个 shell 提示符。试试 ls、top、vim。
// 然后想想：这和直接运行 bash 有什么不同？
//
// 提示：你的 shell 现在跑在一个 PTY 里。
// 这就是 tmux 最核心的一步。

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pty.h>
#include <sys/wait.h>
#include <poll.h>

int main(void) {
    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);

    if (pid < 0) {
        perror("forkpty");
        return 1;
    }

    if (pid == 0) {
        // 子进程：在 PTY slave 端运行 shell
        // 它以为自己连着一个真实终端
        execlp("bash", "bash", NULL);
        perror("exec");
        return 1;
    }

    // 父进程：在 stdin 和 master_fd 之间搬运数据
    // 这就是 tmux client 做的事情（的极简版）
    struct pollfd fds[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = master_fd,    .events = POLLIN },
    };

    char buf[4096];
    while (1) {
        if (poll(fds, 2, -1) < 0) break;

        // 键盘 → master_fd（转发给 shell）
        if (fds[0].revents & POLLIN) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            write(master_fd, buf, n);
        }

        // master_fd → 屏幕（显示 shell 的输出）
        if (fds[1].revents & POLLIN) {
            int n = read(master_fd, buf, sizeof(buf));
            if (n <= 0) break;
            write(STDOUT_FILENO, buf, n);
        }
    }

    int status;
    waitpid(pid, &status, 0);
    return 0;
}
