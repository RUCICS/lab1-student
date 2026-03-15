# Lab 1: mini-tmux 终端复用器

## 1 热身

### 1.1 什么是终端复用器

打开你的终端，运行一条编译命令，编译还没结束你又想查看日志，怎么办？再开一个终端窗口？那如果你通过 SSH 连接远程服务器，网络断开后进程就被杀掉了，工作全部丢失，又怎么办？终端复用器（Terminal Multiplexer）就是为了解决这些问题而存在的：它在一个终端窗口里创建多个独立的"虚拟终端"，每个虚拟终端运行自己的程序，互不干扰。更关键的是，终端复用器在后台持续运行，即使你断开连接，里面的程序也不会中断，重新连上就能恢复原来的工作现场。

现在，请动手试试：

```bash
# Ubuntu / Debian
sudo apt install tmux

# macOS
brew install tmux
```

安装完成后，运行 `tmux`，你会进入一个看起来和普通终端差不多的环境。试试以下操作：

1. 按 `Ctrl+B`，松开后按 `%`，屏幕会垂直分成两半，每半边是一个独立的 Pane
2. 按 `Ctrl+B`，松开后按方向键，在两个 Pane 之间切换焦点
3. 在一个 Pane 里运行 `top`，切到另一个 Pane 运行 `ls`，观察两边互不影响
4. 按 `Ctrl+B`，松开后按 `d`，你会被"弹出"回原来的终端，但 tmux 还在后台运行
5. 运行 `tmux attach`，你又回到了刚才的工作现场

花 10 分钟认真玩一玩。你对 tmux 建立的直觉，会成为后面实现 mini-tmux 的重要参照。

### 1.2 你将要做什么

你将从零实现一个简化版终端复用器 mini-tmux。它具备 tmux 的核心架构：后台 Server 进程管理多个伪终端窗格（Pane），前台 Client 进程负责显示和接收用户输入，两者通过 Unix domain socket 通信。你需要实现 Pane 的创建、销毁、焦点切换、信号精确投递、断开重连（Detach/Reattach）、多 Client 同时连接、输出日志、输出管道等功能。

这不是一个玩具项目。实现过程中你会深入接触进程管理、伪终端、信号机制、socket 通信、I/O 多路复用等操作系统核心概念。这些概念在课本上可能只有几行定义，但在 mini-tmux 中它们会以错综复杂的方式交织在一起。

### 1.3 语言与编译要求

使用 C 或 C++（C11/C++17 或更高版本）实现，编译产物为单个可执行文件 `mini-tmux`。不得依赖第三方库（标准库和 POSIX API 除外）。

项目根目录需提供 `Makefile`，满足：

```bash
make            # 编译生成 ./mini-tmux
make clean      # 清理编译产物
```

## 2 前置知识路线图

这一部分按照实现顺序列出你需要掌握的核心概念。对于每个概念，我们只给出一句话的说明、它与 mini-tmux 的关系、一两个思考问题以及权威参考资料。概念本身需要你自行学习，可以阅读参考资料，也可以向 AI 提问，但思考问题请务必自己想清楚。

### 2.1 PTY 伪终端

伪终端（Pseudo-terminal, PTY）是一对互相连通的字符设备，一端叫 master，一端叫 slave，程序写入 master 的数据会从 slave 读出，反之亦然，模拟了一个"真实终端"的行为。

**与 mini-tmux 的关系**：每个 Pane 里运行的程序（比如 shell、vim、top）都以为自己连着一个真实终端，这样它们才能正确处理行编辑、光标移动、颜色输出等功能。mini-tmux 的 Server 通过 PTY 的 master 端读写这些程序的输入输出。如果不用 PTY 而是用普通的 pipe，程序会检测到自己没有连着终端（`isatty()` 返回 false），从而关闭交互功能，退化为纯文本批处理模式。

> 思考：
> 1. 当你在 PTY master 端写入字符 `'a'` 时，slave 端的程序会读到什么？如果写入的是 `Ctrl+C`（即字节 `0x03`），slave 端会发生什么？谁负责处理这个控制字符？
> 2. `openpty()` 和 `posix_openpt()` + `grantpt()` + `unlockpt()` 两种方式都能创建 PTY，它们有什么区别？

**参考资料**：
- `man 7 pty`：PTY 概述
- `man 3 openpty`：创建 PTY 的便捷接口
- APUE（Advanced Programming in the UNIX Environment）第 19 章：Pseudo Terminals
- Linus Akesson, "The TTY demystified"：https://www.linusakesson.net/programming/tty/

### 2.2 进程 进程组与会话

每个进程属于一个进程组（Process Group），多个进程组组成一个会话（Session）。会话有一个控制终端（Controlling Terminal），控制终端上产生的信号会发送给前台进程组（Foreground Process Group）的所有进程。

**与 mini-tmux 的关系**：mini-tmux 的每个 Pane 都需要运行在独立的会话中，拥有独立的控制终端（即 PTY slave）。这样当用户在某个 Pane 中按 `Ctrl+C` 时，SIGINT 只会发送给该 Pane 的前台进程组，不会影响其他 Pane。Server 进程本身也需要调用 `setsid()` 脱离 Client 的控制终端，这样 Client 断开后 Server 不会收到 SIGHUP。

> 思考：
> 1. 在 Pane 子进程中，为什么需要先调用 `setsid()` 再调用 `ioctl(fd, TIOCSCTTY, 0)` 设置控制终端？如果顺序反过来会怎样？
> 2. 当 Pane 里的 shell 启动一个管道命令 `cat file | grep pattern | wc -l` 时，这些进程分别属于哪个进程组？按 `Ctrl+C` 时谁会收到 SIGINT？

**参考资料**：
- `man 2 setsid`：创建新会话
- `man 2 setpgid`：设置进程组
- `man 2 tcsetpgrp` / `man 3 tcsetpgrp`：设置前台进程组
- APUE 第 9 章：Process Relationships
- 蒋炎岩（南京大学）"终端、进程组和 UNIX Shell"：https://www.bilibili.com/video/BV1bNQAYZEpu/ ，这节课覆盖了终端设备、PTY、进程组、Session、前台/后台进程组等概念，和 mini-tmux 高度相关，推荐完整观看

### 2.3 信号语义

信号（Signal）是 Unix 系统中进程间异步通知的机制。不同的信号有不同的默认行为和触发方式，理解以下几个信号对 mini-tmux 的实现至关重要：

| 信号 | 触发方式 | 与 mini-tmux 的关系 |
|------|---------|-------------------|
| SIGINT | 用户按 `Ctrl+C`，终端驱动发送给前台进程组 | 必须只投递给焦点 Pane 的前台进程组 |
| SIGTSTP | 用户按 `Ctrl+Z`，终端驱动发送给前台进程组 | 同上，必须精确投递，不能泄漏到其他 Pane |
| SIGWINCH | 终端窗口大小改变时发送 | 当 Client 终端 resize 时，Server 需要更新各 Pane 的 PTY winsize 并发送此信号 |
| SIGCHLD | 子进程状态改变（退出或停止）时发送给父进程 | Server 需要处理此信号以回收 Pane 中退出的子进程，防止僵尸进程 |
| SIGPIPE | 向已关闭读端的 pipe 写入时发送 | Client 断开后 Server 向对应 socket 写入时可能触发，需要正确处理 |

> 思考：
> 1. SIGINT 是 `Ctrl+C` 时"谁"发出的？是内核？是终端驱动？还是你的 mini-tmux Server？如果 Client 处于 raw mode，`Ctrl+C` 的字节还会触发信号吗？
> 2. 如果 Server 没有处理 SIGPIPE，当 Client 突然断开连接时会发生什么？

**参考资料**：
- `man 7 signal`：信号概述
- APUE 第 10 章：Signals

### 2.4 Unix domain socket

Unix domain socket 是同一台机器上进程间通信的机制，接口与网络 socket 相同（`socket()`、`bind()`、`listen()`、`accept()`、`connect()`），但数据不经过网络协议栈，效率更高。

**与 mini-tmux 的关系**：Server 和 Client 之间通过 Unix domain socket 通信。Server 创建一个 socket 文件监听连接，Client 连接上来后，双方通过这条 socket 双向传输键盘输入和屏幕输出。

> 思考：
> 1. Unix domain socket 的地址是一个文件路径。如果 Server 异常崩溃没有清理 socket 文件，下次启动时 `bind()` 会失败。你打算怎么处理？
> 2. Server 同时需要监听新 Client 的连接请求、已连接 Client 的数据、多个 Pane PTY 的输出，如何做到"同时"？

**参考资料**：
- `man 7 unix`：Unix domain socket 概述
- Beej's Guide to Network Programming：https://beej.us/guide/bgnet/

### 2.5 I/O 多路复用

I/O 多路复用（I/O Multiplexing）让一个线程能同时监听多个文件描述符（File Descriptor, fd）的可读/可写事件，任意一个 fd 就绪时立即处理，避免了为每个 fd 创建一个线程的开销。

**与 mini-tmux 的关系**：mini-tmux 的 Server 需要同时监听多个 fd，包括监听 socket（等待新 Client 连接）、所有已连接 Client 的 socket（接收输入）、所有 Pane 的 PTY master（接收程序输出）。使用 `poll()` 或 `epoll` 构建事件循环（Event Loop）是实现 Server 的核心架构模式。

> 思考：
> 1. `poll()` 和 `epoll` 在 fd 数量较少时性能差异不大，但在 fd 数量很多时 `epoll` 更高效。为什么？它们在内部实现上有什么本质区别？
> 2. 如果 Server 的事件循环中某个回调函数执行时间过长（比如一次性处理大量数据），会对其他 fd 的响应延迟产生什么影响？

**参考资料**：
- `man 2 poll`：poll 系统调用
- `man 7 epoll`：epoll 概述
- `man 2 epoll_create`、`man 2 epoll_ctl`、`man 2 epoll_wait`：epoll 接口

### 2.6 终端 raw mode 与 termios

终端默认工作在 canonical mode（行缓冲模式）：用户输入一行后按回车，程序才能读到。而 raw mode 关闭了行缓冲、回显、信号字符处理等所有终端层的加工，程序能逐字节地读到用户的每一次按键。

**与 mini-tmux 的关系**：mini-tmux 的 Client 必须将自己的终端设置为 raw mode，否则 `Ctrl+B`（前缀键）会被终端驱动处理而不是传给 Client 程序，`Ctrl+C` 会直接杀掉 Client 而不是转发给 Pane。Client 退出时必须恢复终端的原始设置，否则用户的终端会变得无法正常使用。

> 思考：
> 1. `cfmakeraw()` 具体修改了 termios 结构体中的哪些标志位？为什么每个标志位都需要修改？
> 2. 如果 Client 崩溃了（比如收到 SIGSEGV），终端还停留在 raw mode，用户的终端会表现出什么异常？你能设计什么机制来降低这种情况的影响？

**参考资料**：
- `man 3 termios`：终端属性
- `man 3 cfmakeraw`：设置 raw mode
- `man 1 stty`：查看和修改终端属性（调试利器，试试 `stty -a`）

### 2.7 文件描述符 dup2 与 pipe

文件描述符（File Descriptor, fd）是进程访问文件、socket、PTY 等 I/O 资源的整数句柄。`dup2()` 可以将一个 fd 复制到指定的 fd 编号上（通常用于将 PTY slave 重定向到 stdin/stdout/stderr）。`pipe()` 创建一对 fd，一端写入的数据可以从另一端读出。

**与 mini-tmux 的关系**：创建 Pane 时，子进程需要用 `dup2()` 将 PTY slave 的 fd 设置为自己的 stdin（fd 0）、stdout（fd 1）、stderr（fd 2），然后关闭多余的 fd，再 `exec` shell。`:pipeout` 命令需要 `pipe()` 创建管道，将 Pane 输出同时写入外部命令的 stdin。fd 泄漏（忘记关闭不需要的 fd）是 mini-tmux 实现中最常见的 bug 之一。

> 思考：
> 1. `fork()` 之后，子进程继承了父进程的所有 fd。在子进程中执行 `dup2(slave_fd, STDIN_FILENO)` 之后，为什么还需要关闭原来的 `slave_fd`？如果不关闭会怎样？
> 2. 如果 Server 在 `fork()` 之后忘记在父进程中关闭 PTY slave 的 fd，会产生什么后果？（提示：考虑 Pane 中的 shell 退出时 master 端的行为。）

**参考资料**：
- `man 2 dup2`：复制文件描述符
- `man 2 pipe`：创建管道
- `man 2 close`：关闭文件描述符
- APUE 第 3 章：File I/O

### 2.8 动手实验建议

在开始实现 mini-tmux 之前，建议你先写几个小的测试程序来验证自己对上述概念的理解。例如：

1. 写一个程序，调用 `openpty()` 创建 PTY，`fork()` 子进程，子进程在 PTY slave 上运行 `/bin/bash`，父进程从 PTY master 读输出并写入输入。验证 `isatty()` 的返回值
2. 用 `strace` 跟踪 tmux 的启动过程：`strace -f -e trace=openat,ioctl,clone,setsid tmux`，观察它创建 PTY 和设置会话的系统调用序列
3. 写一个简单的 echo server，使用 Unix domain socket 和 `poll()` 同时处理多个客户端连接

这些小实验能帮你在一个简单的环境中验证每个概念，避免在 mini-tmux 的复杂环境中同时面对太多未知。

## 3 功能需求

本部分按照建议的实现顺序组织。先让最简单的情况跑通，再逐步增加复杂性。每个阶段都有明确的可验证目标，你可以在实现完一个阶段后停下来测试，确认正确后再继续。

每个小节开头会给你一些可以在 tmux 上动手验证的操作。请务必先做一遍，建立直觉之后再看具体要求。

### 3.1 架构概述

在开始写代码之前，先来观察 tmux 的进程结构。打开一个终端，启动 tmux，然后在另一个终端中运行：

```bash
ps aux | grep tmux
```

你会看到两个进程：一个是 `tmux: server`，一个是 `tmux: client`。Server 是后台常驻的，Client 是你当前的交互窗口。试试 `tmux detach`（或 `Ctrl+B` + `d`）之后再看 `ps aux | grep tmux`，Client 消失了，但 Server 还在。再 `tmux attach`，一个新的 Client 出现了。

这就是 mini-tmux 的核心架构：

**Server 进程**：后台常驻，管理所有 Pane 及其关联的 PTY。负责 fork 子进程、分配 PTY、转发 I/O、投递信号。支持多个 Client 同时连接。Server 在没有 Client 连接时继续运行，Pane 中的程序不受影响。

**Client 进程**：前台交互，连接到 Server，显示当前窗格布局并接收用户输入。可以有多个 Client 同时 attach。

它们之间靠什么通信？看看文件系统：

```bash
ls -la /tmp/tmux-$(id -u)/
```

你会看到一个 socket 文件。Server 和 Client 就是通过这个 Unix domain socket 通信的。你的 mini-tmux 也要这样做。

你需要自己设计 Server 与 Client 之间的通信协议，这是一个重要的设计决策：Server 是直接转发各 PTY 的原始字节流让 Client 自己渲染，还是在 Server 端完成渲染后推送结构化的屏幕数据？两种方案各有优劣，请自行权衡。

Server 的核心是一个事件循环（Event Loop），使用 `poll()` 或 `epoll` 同时监听所有需要关注的 fd（监听 socket、Client socket、PTY master 等），在任意 fd 就绪时进行处理。

### 3.2 命令行接口

```
./mini-tmux              # 首次启动：创建 Server + 自动 attach Client
./mini-tmux attach       # 连接到已有 Server（读写模式）
./mini-tmux attach -r    # 连接到已有 Server（只读模式）
```

单一可执行文件。首次运行时，内部 fork 出 Server 进程（后台），然后当前进程作为 Client attach 上去。

Server 实例通过环境变量 `MINI_TMUX_SERVER` 区分。如果该变量已设置，使用其值作为实例名称（用于 socket 路径等）；如果未设置，使用默认名称。这允许同一台机器上运行多个独立的 mini-tmux 实例。

### 3.3 单 Pane 基础

这是你应该首先实现的最小可运行版本。目标：启动 mini-tmux 后，看到一个 shell 提示符，能正常输入命令并看到输出，效果和普通终端几乎一样。

先在 tmux 里感受一下"正确"长什么样。启动 tmux 后运行这几条命令，观察输出：

```bash
# 在 tmux 的 pane 里
tty                    # 会显示 /dev/pts/X，而不是你外部终端的设备
isatty() 的等价检查：
python3 -c "import os; print(os.isatty(0), os.isatty(1), os.isatty(2))"
# 应该全部是 True

stty size              # 显示 pane 的行列数
echo $TERM             # 通常是 screen 或 xterm-256color
```

如果你的 mini-tmux 也能通过这些检查，说明 PTY 接线基本正确。

具体要求：

1. **PTY 创建**：Server 创建一个 PTY master-slave 对，fork 子进程，子进程调用 `setsid()` 创建新会话，将 PTY slave 设为控制终端，用 `dup2()` 重定向 stdin/stdout/stderr 到 PTY slave，然后 `exec` 默认 shell
2. **双向 I/O 转发**：Server 从 PTY master 读到程序输出后，转发给 Client；从 Client 收到用户输入后，写入 PTY master
3. **raw mode**：Client 启动时将自己的终端设为 raw mode，退出时恢复原始设置
4. **isatty 正确性**：Pane 内程序的 stdin、stdout、stderr 必须是 TTY（`isatty()` 返回 true）
5. **窗口大小**：Client 将自己的终端大小通过 socket 告知 Server，Server 通过 `ioctl(TIOCSWINSZ)` 设置 PTY 的 winsize

首次启动时自动创建 Pane 0。

### 3.4 命令模式

先在 tmux 里试一下命令模式的手感。按 `Ctrl+B` 然后按 `:`，你会看到底部出现一个冒号提示符。输入 `split-window` 然后回车，屏幕被分成了上下两半。再按 `Ctrl+B` + `:`，输入 `kill-pane`，刚创建的 pane 被关掉了。

mini-tmux 的命令模式类似，但命令名称更简单：

| 命令 | 说明 |
|------|------|
| `:new` | 创建新 Pane（启动默认 shell） |
| `:kill <pane_id>` | 销毁指定 Pane |
| `:focus <pane_id>` | 切换焦点到指定 Pane |

**命令模式的进入方式**：按 `Ctrl+B` 后按 `:` 进入命令模式（与 tmux 行为一致），此时用户输入的内容不会转发给 Pane，而是作为命令处理。按回车执行命令，按 Escape 取消。

**Pane ID 规则**：Pane ID 从 0 开始递增编号。首次启动时自动创建 Pane 0。已销毁的 Pane ID 不复用。例如：创建 Pane 0、1、2，销毁 Pane 1 后再创建新 Pane，新 Pane 的 ID 是 3 而非 1。

**最后一个 Pane 退出的行为**：当最后一个 Pane 中的子进程退出（或被 `:kill` 销毁且没有其他 Pane 存在）时，Server 应当清理资源并退出，所有 attached Client 也应当退出。

### 3.5 屏幕布局

在 tmux 中按 `Ctrl+B` + `%` 垂直分割（或 `"` 水平分割），观察屏幕如何被分成两半。试试在两个 pane 中分别运行 `stty size`，你会发现它们报告的行列数加起来大约等于总行数（减去分隔行）。再试试拖动终端窗口改变大小，两边的 `stty size` 都会跟着变。

mini-tmux 只需要实现最简单的情况：水平分割（上下排列），垂直等分。

当存在多个 Pane 时，Client 必须将终端屏幕分割，同时显示所有 Pane 的内容。

- **布局方式**：垂直等分（每个 Pane 占若干行，Pane 之间用分隔行区分）。不要求水平分割。
- **分隔行**：Pane 之间用一行分隔（内容自定，例如 `--- pane 0 ---`）。分隔行不计入 Pane 的可用行数。
- **Pane 可用行数**：终端总行数减去分隔行数，再平均分配给各 Pane。余数分配给哪个 Pane 不做要求。
- **焦点标记**：在分隔行或其他视觉元素上标记当前焦点 Pane（形式不限，能区分即可）。
- **winsize 联动**：每个 Pane 的 PTY winsize 应反映其实际可用行列数（行数为分配到的行数，列数为终端宽度）。Pane 数量变化或终端 resize 时，重新计算布局并更新所有 Pane 的 winsize（通过 `ioctl(TIOCSWINSZ)` 设置后发送 SIGWINCH）。

### 3.6 焦点切换与信号投递

这是 mini-tmux 最微妙的部分之一。先来观察 tmux 中信号隔离的行为：

```bash
# 在 tmux 中创建两个 pane（Ctrl+B % 垂直分割）
# 左边 pane 运行：
cat     # cat 会等待输入，按 Ctrl+C 会被 SIGINT 杀掉

# 切换到右边 pane（Ctrl+B 方向键），运行：
sleep 999

# 现在在右边 pane 按 Ctrl+C
# 观察：sleep 被杀掉了，但左边的 cat 完全不受影响
```

为什么？因为每个 pane 运行在独立的 Session 中，`Ctrl+C` 产生的 SIGINT 只发送给当前控制终端的前台进程组。你可以用 `ps -o pid,pgid,sid,tty,comm` 来验证不同 pane 的进程确实属于不同的 Session。

**焦点切换**有两种方式：

1. **命令模式**：`:focus <pane_id>` 直接跳转到指定 Pane
2. **前缀键快捷键**：`Ctrl+B` 后按方向键
   - `Ctrl+B` + `n`：切换到下一个 Pane
   - `Ctrl+B` + `p`：切换到上一个 Pane

只有获得焦点的 Pane 接收用户键盘输入。焦点状态在 Server 侧维护，所有 Client 共享同一个焦点 Pane。

**信号精确投递**：

- **SIGINT（Ctrl+C）** 和 **SIGTSTP（Ctrl+Z）** 只投递给当前焦点 Pane 的前台进程组，不泄漏到其他 Pane。这要求每个 Pane 的子进程运行在独立的会话中，且 `tcsetpgrp()` 正确设置了前台进程组。
- **进程组隔离**：每个 Pane 的子进程运行在独立的 Session 中。验证方法：在两个 Pane 分别运行程序，只在一个 Pane 中按 `Ctrl+C`，另一个 Pane 的程序不受影响。

### 3.7 Detach 与 Reattach

Detach/Reattach 是终端复用器存在的最根本理由。试试这个：

```bash
tmux                       # 启动 tmux
ping localhost             # 运行一个持续输出的命令
# Ctrl+B d                 # detach

# 你回到了原来的终端，但 ping 还在后台跑着
tmux attach                # 重新连上，ping 的输出还在继续
```

对于 SSH 用户来说，这意味着网络断开不会丢失工作。这也是为什么你的 Server 必须独立于 Client 存活。

mini-tmux 的行为：

- `Ctrl+B` 后按 `d`：当前 Client detach（断开），其他 Client 不受影响，Server 和所有 Pane 继续运行
- `./mini-tmux attach`：新 Client 连接到 Server

详细行为要求：

- Detach 期间 Pane 中的程序正常运行，输出由 Server 缓存
- Reattach 后 Client 能看到当前的屏幕内容
- Client 异常断开（进程被 kill 或连接中断）时，Server 不退出，等效于 detach
- 快速反复 detach/attach 不应导致 fd 泄漏或进程泄漏
- Detach 期间 `:log` 和 `:pipeout` 继续工作（它们是 Server 侧行为，不依赖 Client）

### 3.8 多 Client 支持

你可以在 tmux 上直接体验多 Client。打开两个终端窗口，在第一个窗口启动 `tmux`，在第二个窗口运行 `tmux attach`。现在两个窗口显示同样的内容，在任意一个窗口输入命令，另一个窗口实时同步。这在结对编程或远程教学时非常有用。

Server 支持多个 Client 同时 attach 到同一个 session：

- **输出广播**：所有 Pane 的输出同时发送给每个 attached Client
- **输入路由**：只有非只读 Client 可以发送输入。如果有多个读写 Client，所有输入都转发给焦点 Pane（类似 tmux 默认行为）
- **只读 Client**：`attach -r` 的 Client 只接收输出，不能发送按键或命令。Server 忽略只读 Client 的所有输入
- **独立 Detach**：一个 Client detach 不影响其他 Client。Server 只在所有 Client 都断开后进入无 Client 状态
- **终端大小**：当多个 Client attached 时，Server 将 Pane 的 winsize 设置为所有 attached Client 终端大小的最小值（行数取最小，列数取最小）。Client attach 或 detach 时重新计算并更新所有 Pane 的 winsize，发送 SIGWINCH
- **Client 异常断开**：Client 进程被 kill 或连接中断时，Server 不退出，等效于该 Client detach

### 3.9 高级命令

tmux 的 `capture-pane` 和 `pipe-pane` 是非常实用的调试工具。试试：

```bash
# 在 tmux 中
# Ctrl+B :  然后输入：
pipe-pane -o "cat >> /tmp/pane-log.txt"
# 现在这个 pane 的所有输出都会同时追加到 /tmp/pane-log.txt
ls -la
# 在另一个终端查看：cat /tmp/pane-log.txt，你会看到 ls 的输出

# 再试 capture-pane：
# Ctrl+B :  然后输入：
capture-pane -p > /tmp/screen.txt
# 打开 /tmp/screen.txt，里面是当前 pane 的屏幕快照
```

mini-tmux 实现类似的功能，但命令名称更直观。

#### 3.9.1 :log 输出日志

`:log <pane_id> <file_path>` 将指定 Pane 的 PTY 输出追加写入文件。Server 在收到命令后打开文件（追加模式），后续该 Pane 所有输出同时写入文件。

- `:log-stop <pane_id>` 停止并关闭文件
- Pane 被 `:kill` 时自动停止 log 并关闭文件
- 同一 Pane 重复 `:log` 替换之前的 log 目标（关闭旧文件，打开新文件）
- Detach 期间 log 继续工作

#### 3.9.2 :pipeout 输出管道

`:pipeout <pane_id> <cmd>` 将指定 Pane 的 PTY 输出实时 pipe 给外部命令。Server fork 子进程执行 `<cmd>`（通过 `/bin/sh -c`），将 PTY master 读到的数据同时写入子进程的 stdin。

- `:pipeout-stop <pane_id>` 手动停止：关闭 pipe 写端，等待子进程退出
- Pane 被 `:kill` 时自动清理 pipe 和子进程
- 外部命令自行退出时自动清理（Server 通过 SIGCHLD/waitpid 感知）
- 同一 Pane 同时只能有一个 pipeout，重复 `:pipeout` 先停止旧的再启动新的
- Detach 期间 pipeout 继续工作

#### 3.9.3 :capture 屏幕捕获

`:capture <pane_id> <file_path>` 将指定 Pane 当前的屏幕内容导出到文件。

- Server 需要为每个 Pane 维护输出缓冲区（至少保留最近 1000 行）
- 导出的内容应包含最近的输出，不含 ANSI 转义序列（纯文本）
- 多次 `:capture` 同一 Pane 到同一文件，文件内容为最新快照（覆盖模式）

### 3.10 进程管理与资源清理

这不是一个独立的功能模块，而是贯穿整个实现的要求，也是最容易出 bug 的地方。你可以用以下命令来检查自己的 mini-tmux 是否存在资源泄漏：

```bash
# 检查僵尸进程
ps aux | grep mini-tmux | grep -v grep
# 如果看到状态为 Z (zombie) 的进程，说明没有正确 waitpid()

# 检查 fd 泄漏（Linux）
ls -la /proc/$(pgrep -f "mini-tmux.*server")/fd/ | wc -l
# 创建和销毁几个 pane 后，fd 数量应该回到初始值

# 检查 socket 文件残留
ls /tmp/mini-tmux-*
# Server 退出后不应该留下 socket 文件
```

Server 在运行过程中会创建大量子进程（Pane shell、pipeout 命令等）和 fd（PTY、socket、pipe、日志文件等），必须严格管理它们的生命周期。

- **僵尸回收**：Pane 中的子进程退出后，Server 必须通过 `waitpid()` 及时回收，不留下僵尸进程（Zombie Process）。推荐在 SIGCHLD handler 中或事件循环每次迭代时调用 `waitpid(-1, &status, WNOHANG)` 循环回收
- **fd 泄漏预防**：`fork()` 后父子进程各自关闭不需要的 fd。Pane 销毁时关闭对应的 PTY master。Client 断开时关闭对应的 socket。`:log-stop` 和 `:pipeout-stop` 时关闭对应的文件/管道
- **进程组隔离**：每个 Pane 的子进程运行在独立的 Session 中，`tcsetpgrp()` 正确设置前台进程组

### 3.11 命令汇总表

为方便查阅，这里汇总所有命令模式的命令和前缀键操作：

**命令模式命令**（`Ctrl+B` + `:` 进入命令模式后输入）：

| 命令 | 说明 |
|------|------|
| `:new` | 创建新 Pane（启动默认 shell） |
| `:kill <pane_id>` | 销毁指定 Pane |
| `:focus <pane_id>` | 切换焦点到指定 Pane |
| `:log <pane_id> <file_path>` | 将 Pane 输出追加写入文件 |
| `:log-stop <pane_id>` | 停止 log |
| `:pipeout <pane_id> <cmd>` | 将 Pane 输出实时 pipe 给外部命令 |
| `:pipeout-stop <pane_id>` | 停止 pipeout |
| `:capture <pane_id> <file_path>` | 导出 Pane 当前屏幕内容到文件 |

**前缀键操作**（按 `Ctrl+B` 后松开，再按对应键）：

| 按键 | 说明 |
|------|------|
| `d` | Detach 当前 Client |
| `n` | 切换到下一个 Pane |
| `p` | 切换到上一个 Pane |
| `:` | 进入命令模式 |

## 4 评测与提交

### 4.1 评测机制概述

评测使用自动化 Harness。Harness 不通过你的代码内部接口来检查正确性，而是采用"双端探针 + 侧信道"的架构：Harness 提供一个预编译的探针程序（Probe），在你的 Pane 中运行。Probe 是一个普通的命令行程序，读 stdin、写 stdout、注册信号 handler。它通过一条独立的侧信道（Sideband Channel，不经过你的 Server）直接向 Harness 报告环境自检结果。

**你不需要关心 Probe 的实现细节。** 只需确保 Pane 能正确运行任意二进制程序，PTY 管道正确接线即可。如果你的 PTY 接线正确，Probe 自然能正常工作。

Probe 的运行方式：Harness 通过模拟键盘输入，在你的 Pane 的 shell 中执行类似如下的命令：

```
/path/to/probe /tmp/sideband.sock session_0
```

Probe 通过侧信道报告的信息包括：
- 环境自检（`isatty()` 结果、窗口大小、进程 ID 和进程组 ID）
- 信号接收情况（收到了哪些信号）
- I/O 数据透传正确性（随机 token 的发送和接收）

### 4.2 公开测试用例

项目仓库中的 `workloads/public/` 目录包含所有公开测试用例的 YAML 描述文件。你可以直接阅读这些文件来了解每个测试用例在测什么、执行了哪些步骤。

例如，`01_single_pane_basic.yaml` 描述了一个单 Pane 基础 I/O 测试：启动 mini-tmux，在 Pane 中启动 Probe，验证环境自检通过，验证输出和输入 token 能正确透传。

公开测试用例覆盖以下维度：

| 类别 | 测试内容 |
|------|---------|
| 基础 I/O | 单 Pane 环境自检、输入输出 token 透传 |
| 信号隔离 | 多 Pane 下 Ctrl+C / Ctrl+Z 只影响焦点 Pane |
| 窗格管理 | 创建、销毁、快速创建销毁循环 |
| 进程管理 | 僵尸回收、进程组隔离 |
| Resize | 窗口大小变化时正确发送 SIGWINCH 并更新 winsize |
| 屏幕布局 | 多 Pane 同时可见，winsize 与布局联动 |
| 压力测试 | 8 Pane 并发、高频输出、TUI 程序兼容性 |
| 会话管理 | Detach/Reattach，Client 异常断开后 Server 存活 |
| 输出日志 | `:log` 基础功能 |
| 输出管道 | `:pipeout` 基础功能，外部命令退出自动清理 |
| 多 Client | 多个 Client 同时 attach，输出广播，只读 Client |
| 屏幕捕获 | `:capture` 导出 Pane 内容 |
| Server 生命周期 | 最后一个 Pane 退出后 Server 清理退出 |

你可以阅读 YAML 文件来精确了解每个测试的步骤和验证点，但不能在本地运行自动评测。评测只能通过提交后在服务端运行。

### 4.3 评分规则

- 每个测试用例 Pass/Fail
- 总分为各用例加权通过率
- 权重按类别分配：信号隔离和会话管理类权重较高，压力测试类权重较低
- `make grade` 只返回总分（格式：`Score: X/Y`），不返回每个用例的详细结果
- **Private 盲测**：学期内进行两次（中期检查和最终提交各一次），使用与 Public 同分布但更多实例、更长运行时间的测试用例，只返回总分。Private 不引入任何新的负载类型或测试维度

### 4.4 开发工具

项目 Makefile 提供以下辅助命令：

```bash
make            # 编译生成 ./mini-tmux
make clean      # 清理编译产物
make run        # 在 Docker 中编译并运行 mini-tmux（Linux 环境）
make shell      # 进入 Docker 环境的 shell（可手动测试）
make grade      # 通过 Docker 运行公开测试用例并评分
```

`make run` 和 `make shell` 让你在 Linux Docker 容器中开发和测试，确保与评测环境一致。如果你在 macOS 上开发，强烈建议使用这两个命令来验证你的实现，因为 PTY 和信号的行为在 macOS 和 Linux 之间存在细微差异。

### 4.5 提交方式

通过 GitHub Classroom 提交。将你的代码推送到分配的仓库即可。确保 `make` 能在干净的 Linux 环境中编译成功。

## 5 Presentation

这个实验的 Presentation 核心不是 mini-tmux 本身，而是你在整个过程中如何学习、如何与 AI 协作。请在做实验的过程中有意识地记录以下内容，具体形式和要求我们后续通知。

**记录方向**：

1. **你用 AI 做了什么**：哪些部分让 AI 生成的，哪些是自己写的，为什么这样分工
2. **AI 协作中的经验和教训**：AI 在哪里帮了大忙，在哪里给出了错误方案，你是怎么发现和修正的
3. **你对系统概念的理解**：通过这个实验你学到了什么，哪些概念是之前完全不知道的
