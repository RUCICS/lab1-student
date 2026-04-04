# Lab 1: mini-tmux 终端复用器

> Designed by [@andylizf](https://github.com/andylizf) with [@Nictheboy](https://github.com/Nictheboy) and [@Cheese-2023](https://github.com/Cheese-2023)

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

你将从零实现一个简化版终端复用器 mini-tmux。它具备 tmux 的核心架构：后台 Server 进程管理伪终端（PTY）和 shell 子进程，前台 Client 进程负责显示和接收用户输入，两者通过 Unix domain socket 通信。基础部分需要实现：单 Pane 的 PTY 管理、Client-Server 分离、断开重连（Detach/Reattach）、多 Session 信号隔离、多 Client 同时连接。在此基础上，Bonus 功能包括多 Pane 屏幕分割、窗口大小变化（SIGWINCH）、输出日志、输出管道、屏幕捕获等。

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

```
PTY 的本质：一对互通的端点

  write() ──►┌────────┐         ┌────────┐◄── read()
             │ master │ ~~~~~~~ │ slave  │
  read()  ◄──┘────────┘         └────────┘──► write()

  你的程序持有 master fd          子进程（shell/vim）持有 slave fd
```

在一个普通终端中，数据流是这样的：

```
没有 tmux 时：

  键盘 ──► terminal ──► master fd ···PTY··· slave fd ──► shell
  屏幕 ◄── terminal ◄── master fd ···PTY··· slave fd ◄── shell
```

**与 mini-tmux 的关系**：每个 Pane 里运行的程序（比如 shell、vim、top）都以为自己连着一个真实终端，这样它们才能正确处理行编辑、光标移动、颜色输出等功能。mini-tmux 的 Server 通过 PTY 的 master 端读写这些程序的输入输出。如果不用 PTY 而是用普通的 pipe，程序会检测到自己没有连着终端（`isatty()` 返回 false），从而关闭交互功能，退化为纯文本批处理模式。

tmux 做的事情就是在 terminal 和 PTY 之间插入一层 Client-Server：

```
有 tmux 时：

  键盘 ──► terminal ──► Client ═══socket═══ Server ──► master fd ··· slave fd ──► shell
  屏幕 ◄── terminal ◄── Client ═══socket═══ Server ◄── master fd ··· slave fd ◄── shell

  Client 断开？Server 和 shell 继续运行。重新 attach 就回来了。
```

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

信号（Signal）是 Unix 系统中进程间异步通知的机制。mini-tmux 需要正确处理以下几个信号：

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

Server 的事件循环需要同时监听所有这些 fd：

```
poll() / epoll 监听的 fd 集合：

  ┌─ listen_fd        （新 Client 连接）
  ├─ client_fd[0..n]  （已连接 Client 的输入）
  └─ pty_master[0..m] （各 Pane 程序的输出）

  任意一个 fd 就绪 → 立即处理 → 回到 poll 等待
```

> 思考：
> 1. `poll()` 和 `epoll` 在 fd 数量较少时性能差异不大，但在 fd 数量很多时 `epoll` 更高效。为什么？它们在内部实现上有什么本质区别？
> 2. 如果 Server 的事件循环中某个回调函数执行时间过长（比如一次性处理大量数据），会对其他 fd 的响应延迟产生什么影响？

**参考资料**：
- `man 2 poll`：poll 系统调用
- `man 7 epoll`：epoll 概述
- `man 2 epoll_create`、`man 2 epoll_ctl`、`man 2 epoll_wait`：epoll 接口

### 2.6 终端 raw mode 与 termios

如果你写过普通的 C/C++ 程序，你会知道：在默认的模式下，你在运行程序时，键盘的输入在回车前是无法被程序读取到的。例如，如果你写了：

```c
scanf("%d %d %d", &a, &b, &c);
```

即使你已经输入了 `1 2 3 `，在按下回车前，scanf 依然不认为你已经输入完成了。

然而，如果你使用 linux 的 shell，你会发现：即使你还没有输入完，当你按 tab 时，shell 会自动补全你输入的内容。显然，与你自己的 C/C++ 程序不同，在你按下回车前，shell 就已经知道你输入了一半的内容了。

此外，当你执行 `sudo apt update` 等命令并输入密码时，你在屏幕上看不到你输入的密码。这也与你在 C/C++ 程序或 shell 中输入字符时的行为不同。

这些不同的表现，究其原因，是因为你的 C/C++ 程序和 shell 程序连接的 tty 设备工作在不同的模式下。

具体来说，tty 有两个可以开启或关闭的特性：**行缓冲**和**回显**。

行缓冲指的是：当你在终端输入字符时，这些字符不会立即被程序读取，而是先被存储在一个缓冲区中。只有当你按下回车时，缓冲区中的所有字符才会被一次性读取。行缓冲模式也被称为 canonical 模式，关闭了行缓冲的模式也被称为 raw 模式。

回显（echo）指的是：当你在终端输入字符时，这些字符会立即被显示在屏幕上。具体来说，回显被开启意味着：当 terminal 程序对 master fd 使用 write() 写入一个字符后，如果 terminal 对 master fd 进行 read()，则这个字符会被立即读取到，即使 slave fd 没有被写入任何字符。

| 场景 | master write → master read? | 原因 |
|------|----------------------------|------|
| canonical + ECHO 开启（默认） | **是**，普通字符被回显 | N_TTY 的 echo 机制 |
| canonical + ECHO 关闭（如密码输入） | **否** | `stty -echo` 关闭了回显 |
| raw 模式 + ECHO 关闭（如 vim） | **否** | 应用程序自己决定输出什么 |
| raw 模式 + ECHO 开启（罕见） | **是** | 即使是 raw 模式，ECHO 标志仍然独立生效 |

对比两种模式下按键 'a' 的数据流：

```
shell（raw + no echo）：
  键盘 'a' ──► master fd ··PTY·· slave fd ──► shell 收到 'a'
                                                shell 自己决定输出什么
                                                shell write() ──► slave fd ··PTY·· master fd ──► 屏幕

scanf 程序（canonical + echo）：
  键盘 'a' ──► master fd ──┬── PTY 线路规程自动回显 'a' ──► master fd ──► 屏幕
                           └── 存入行缓冲，等回车
  键盘 '\n' ──► 行缓冲一次性发给 slave fd ──► scanf() 返回
```

shell 通过 readline 等库自己处理回显（为了支持 tab 补全等），所以工作在 raw + no echo 模式。普通 C 程序默认工作在 canonical + echo 模式。

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

```
dup2(slave_fd, STDIN_FILENO) 的效果：

  修改前                          修改后
  fd 0 ──► stdin 设备             fd 0 ──► slave_fd（PTY slave）
  fd 1 ──► stdout 设备            fd 1 ──► stdout 设备
  fd 3 ──► slave_fd               fd 3 ──► slave_fd（之后应 close）

  对 stdout、stderr 也做同样的 dup2，子进程的全部 IO 就都走 PTY 了。
```

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

当你打开两个终端窗口，各自运行 tmux attach，会形成这样的结构：

```
Terminal 1                                          Terminal 2
┌──────────┐                                       ┌──────────┐
│ Client 1 │──┐                                 ┌──│ Client 2 │
│ 显示+输入 │  │    Unix domain socket           │  │ 显示+输入 │
└──────────┘  │  ┌──────────────────────────┐   │  └──────────┘
              ├──│        Server            │───┤
                 │  ┌─ PTY ─┐  ┌─ PTY ─┐   │
                 │  │ bash  │  │  vim   │   │
                 │  └───────┘  └───────┘   │
                 └──────────────────────────┘
                 后台常驻，Client 断开不影响
```

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

### 3.4 命令模式（Bonus: 多 Pane）

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

### 3.5 屏幕布局（Bonus: 多 Pane）

在 tmux 中按 `Ctrl+B` + `%` 垂直分割（或 `"` 水平分割），观察屏幕如何被分成两半。试试在两个 pane 中分别运行 `stty size`，你会发现它们报告的行列数加起来大约等于总行数（减去分隔行）。再试试拖动终端窗口改变大小，两边的 `stty size` 都会跟着变。

mini-tmux 只需要实现最简单的情况：所有 Pane 上下排列，等分屏幕高度。不需要实现左右分割。

当存在多个 Pane 时，Client 必须将终端屏幕分割，同时显示所有 Pane 的内容。

- **布局方式**：所有 Pane 上下排列（每个 Pane 占若干行，Pane 之间用分隔行区分）。不要求左右分割。
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

### 3.9 高级命令（Bonus）

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

除 Probe 外，`helpers/` 目录还包含 `fork_exit`，它是僵尸进程回收测试的辅助程序。`fork_exit` 在 Pane 中运行时会快速 fork 出多个子进程并让它们立即退出，然后通过侧信道报告这些子进程的 PID。Harness 随后检查你的 Server 是否及时回收了这些僵尸进程。和 Probe 一样，你不需要关心它的实现细节，只需确保 Pane 能正确运行任意二进制程序且 Server 正确处理 SIGCHLD / `waitpid()`。

### 4.2 公开测试用例

项目仓库中的 `workloads/public/` 目录包含所有公开测试用例的 YAML 描述文件，`harness/` 目录包含测试运行器。你可以在本地运行公开测试（需要 Linux x86_64 环境）：

```bash
python3 harness/run_tests.py workloads/public/
```

每次 push 也会自动触发 CI 运行基础层的公开测试。YAML 文件的价值在于让你精确了解每个测试用例在测什么、执行了哪些步骤，从而指导你的实现和自测。

例如，`01_single_pane_basic.yaml` 描述了一个单 Pane 基础 I/O 测试：启动 mini-tmux，在 Pane 中启动 Probe，验证环境自检通过，验证输出和输入 token 能正确透传。

公开测试用例覆盖以下维度：

| 类别 | 测试内容 | 基础/Bonus |
|------|---------|-----------|
| 基础 I/O | 单 Pane 环境自检、输入输出 token 透传 | 基础 |
| 高频输出 / TUI 兼容 | 高频输出不丢数据、TUI 程序正常运行 | 基础 |
| 会话管理 | Detach/Reattach，Client 异常断开后 Server 存活 | 基础 |
| 多 Session 信号隔离 | 多 session 下 Ctrl+C / Ctrl+Z 不跨 session | 基础 |
| Session 管理 | session 创建、销毁、快速创建销毁循环 | 基础 |
| 进程管理 | 僵尸回收、进程组隔离 | 基础 |
| 多 Client | 多个 Client 连接同一 session，广播与只读 | 基础 |
| Server 生命周期 | 最后一个 session 退出后 Server 清理退出 | 基础 |
| 压力测试 | 多 session 并发、SIGTSTP/SIGCONT | 基础 |
| Resize (SIGWINCH) | 窗口大小变化时正确发送 SIGWINCH 并更新 winsize | Bonus |
| 多 Pane / 屏幕布局 | 多 Pane 同时可见，winsize 与布局联动 | Bonus |
| 输出日志 | `:log` 基础功能 | Bonus |
| 输出管道 | `:pipeout` 基础功能，外部命令退出自动清理 | Bonus |
| 屏幕捕获 | `:capture` 导出 Pane 内容 | Bonus |

你可以阅读 `workloads/public/` 中的 YAML 文件来精确了解每个测试的步骤和验证点。每个 YAML 文件描述了一个完整的测试场景：启动条件、操作序列（启动 Probe、发送按键、创建 Session 等）和验证断言（环境检查、信号是否送达、token 是否透传等）。仔细阅读这些文件，你会清楚地知道评测在检查什么。

### 4.3 评分规则

总成绩 = min(基础代码 + 报告/Presentation + Bonus, 100)。认真做了就能拿到不错的成绩。

| 组成 | 满分 | 说明 |
|------|-----:|------|
| 基础代码 | 50 | 四阶段自动评测，通过率 x 50 |
| 报告 / Presentation | 50 | 学习过程 + AI 协作质量 |
| Bonus | 最多 25 | 多 Pane、SIGWINCH、Log、Pipeout、Capture 等 |
| **总分上限** | **100** | Bonus 可补基础或报告的缺口，但总分不超过 100 |

例：基础 0 分 + Bonus 全对 = 25 + 报告分。基础 50 + Bonus 20 + 报告 50 = 100（被 cap）。

#### 基础代码（50 分）

自动评测，按四个阶段计分：

| 阶段 | 覆盖功能 | 关键测试 |
|------|---------|---------|
| 单 Pane | PTY 创建、基础 IO、TUI 兼容 | 01, 05, 09, 10 |
| Client-Server | Unix socket 通信、C/S 拆分 | （隐含在所有测试中） |
| Detach/Reattach | Server 持久化、Client 重连 | 13, 14, 21 |
| 多 Session / 多 Client | 多 session 信号隔离、僵尸回收、多 client 同 session 广播/只读 | 02, 03, 04, 06, 08, 11, 12, 18, 19 |

四阶段内所有测试用例等权，得分 = 总通过率 x 50。建议按阶段顺序实现。

#### Bonus（最多 25 分）

多 Pane（含 Layout）、SIGWINCH、Log、Pipeout、Capture。对应测试：07, 15, 16, 17, 20, 22, 23。Bonus 测试等权，得分 = 通过率 x 25。

#### 公开测试 CI

每次 push 到 main 分支时，GitHub Actions 自动运行 16 个公开基础测试，结果显示在 Actions 页面（通过数和每个测试的 pass/fail，不显示分数）。Bonus 测试不在此范围内。

#### 定期盲测

每三天自动运行一次 main 分支的全部测试（公开 + 隐藏，基础 + Bonus），返回每个类别的通过数与得分，不返回失败原因。盲测与公开测试同分布，不引入新的负载类型。

#### 报告或 Presentation（50 分）

二选一（见第 5 节），评价学习过程和 AI 协作质量，不要求代码完美。Presentation 包含现场问答，更容易展示理解深度。

### 4.4 开发工具

项目 Makefile 提供以下命令：

```bash
make            # 编译生成 ./mini-tmux
make clean      # 清理编译产物
```

请在 Linux x86_64 环境下开发和测试（评测环境为 Ubuntu 24.04）。`helpers/` 中的预编译二进制也是 Linux x86_64 格式。

### 4.5 测试策略

仓库内置了测试运行器（`harness/`）和公开测试用例（`workloads/public/`），你可以在本地运行：

```bash
python3 harness/run_tests.py workloads/public/          # 运行全部公开测试
python3 harness/run_tests.py workloads/public/01_*.yaml  # 运行单个测试
```

每次 push 也会通过 CI 自动运行基础层公开测试，结果在 GitHub Actions 页面查看。

除了自动测试，手动验证同样重要：

- `isatty()`、`tty`、`stty size` 是否正确（3.3 节）
- 多 Session 下 `Ctrl+C` 是否只影响目标 Session 的进程（3.6 节）
- Detach 后 `ps aux` 确认 Server 和 Pane 进程还在（3.7 节）
- `/proc/<pid>/fd/` 检查 fd 数量在创建销毁 Pane 后是否回到初始值（3.10 节）

### 4.6 提交方式

通过 GitHub Classroom 提交。将你的代码推送到分配的仓库即可。确保 `make` 能在干净的 Linux 环境中编译成功。

**截止时间**：2026-04-16 23:59。

### 4.7 AI 使用记录

我们鼓励使用 AI，也鼓励你记录与 AI 的协作过程。以下两项记录**不影响成绩**，但我们强烈建议你尽可能提交：**代码归因**（哪行代码是 AI 写的）和**对话记录**（你和 AI 聊了什么）。这些记录帮助我们了解 AI 在实验中的使用情况，也可能作为代码原创性的参考依据。如果工具无法正常工作或你选择不使用 AI，在报告中简要说明即可。

#### 代码归因：git-ai

[git-ai](https://github.com/git-ai-project/git-ai) 是一个 Git 扩展，能自动追踪每一行代码是人写的还是 AI 生成的。它通过 Git Notes 记录归因信息，不影响你的提交历史。

安装和配置：

```bash
# 安装
curl -sSL https://usegitai.com/install.sh | bash

# 在你的仓库中初始化
cd your-repo
git-ai init

# 配置（将对话元数据写入 Git Notes）
git-ai config set prompt_storage notes
```

git-ai 支持 Claude Code、Cursor、GitHub Copilot 等主流工具，安装后自动生效。你可以随时查看归因统计：

```bash
git-ai stats          # 查看 AI 代码占比
git-ai blame file.c   # 查看每行的归因（类似 git blame）
```

提交时确保推送 Git Notes：`git push origin refs/notes/ai`。

#### 对话记录：ai-logs/

建议在仓库中创建 `ai-logs/` 目录，存放与 AI 的对话记录。git-ai 追踪的是"哪行是 AI 写的"，对话记录补充的是"你们聊了什么才写出这行代码"。两者结合才是完整的协作证据。

格式要求：Markdown 或 JSON，能看到你的提问和 AI 的回答即可。文件名建议带日期或序号，比如 `01-pty-setup.md`、`02-signal-handling.md`。

各工具的导出方法：

| 工具 | 导出方式 |
|------|---------|
| Claude Code | 运行 `/export` 命令，或直接复制 `~/.claude/projects/` 下的 `.jsonl` 文件 |
| Cursor | File > Export Chat，或使用 [cursor-chat-export](https://github.com/somogyijanos/cursor-chat-export) 批量导出 |
| GitHub Copilot | VS Code 中 `Ctrl+Shift+P` > `Chat: Export Chat...` 导出 JSON |
| Codex | 会话自动保存在 `~/.codex/sessions/` 下的 `.jsonl` 文件中，直接复制即可 |
| Trae | 暂无完整导出功能，截图保存关键对话或逐条复制到 Markdown 文件 |
| ChatGPT / Claude 网页版 | 手动复制对话，或在设置中导出数据 |
| 其他工具 | 截图或手动复制均可，关键是保留完整上下文 |

**注意**：请在开发过程中持续导出，不要等到最后。部分工具的对话记录会在 30 天后自动删除。不需要美化或整理，原始记录比精心编排的版本更有价值。

## 5 报告或 Presentation（二选一，50 分）

从以下两种形式中选择一种。我们评价的不是 mini-tmux 做得多好，而是你在这个过程中理解了什么、踩了什么坑、有什么真实的思考。

### 评分标准

| 维度 | 分值 | 说明 |
|------|------|------|
| 系统理解 | 25 | 挑 1-2 个你真正理解的概念（比如 PTY 怎么工作、信号为什么要按进程组投递、poll 的事件循环、socket 通信的生命周期），讲清楚它是什么、为什么需要它、你的代码里是怎么用的。不要求面面俱到，要求讲透 |
| 诚实反思 | 15 | 哪些部分是 AI 写的你没看懂？哪里卡住过？什么让你意外？承认不理解比假装理解更有价值。如果你全程让 AI 生成代码，说清楚你在这个过程中观察到了什么、事后回头看理解了多少 |
| 表达质量 | 10 | 结构清晰，言之有物，不灌水。一页讲透一个概念比十页泛泛而谈得分更高 |

### 选项 A：书面报告

提交一份 Markdown 报告（`report/report.md`）。篇幅不限，但请参考上面的评分标准，把精力放在有深度的内容上。

以下内容**不会**得到高分：
- 逐个复述代码功能（“我的 server 用 poll 监听多个 fd”这是描述，不是理解）
- 直接粘贴 AI 对话记录（对话记录交在 `ai-logs/` 里就行，报告要的是你的思考）
- 全文只有“我做了 X”，没有任何踩坑或困惑

### 选项 B：Presentation

现场演示，8-10 分钟，包含 3-5 分钟问答。具体时间安排另行通知。

Presentation 和报告使用相同的评分标准。区别在于，Presentation 包含现场问答环节，你可以通过回答追问来展示文字难以传达的理解深度。因此，Presentation 在”系统理解”和”诚实反思”两个维度上更容易拿到高分。

除了展示你自己的理解，我们鼓励你在 Presentation 中给同学们带走一些实用的东西。你在这次实验中积累的 AI 协作方法论，比知识本身更值得分享：

- 你是怎么把一个模糊的大任务拆解成 AI 能处理的小任务的？
- 你建立了什么样的反馈循环来验证 AI 的输出？（比如写测试、对比行为、逐步缩小问题范围）
- AI 给出错误方案时，你的排查策略是什么？
- 你在哪些环节学会了不信任 AI 的输出，开始自己审查？
- 如果让你指导一个刚开始做这个实验的同学，你会告诉他什么？

把 Presentation 当成一次复盘：你不只是一个写代码的人，你是指挥 AI 完成一个系统工程的人。讲讲你作为”指挥官”学到了什么。

现场可能根据你的内容提问，例如：
- 你的 server 是怎么知道该把输出发给哪个 client 的？
- 如果 client 异常退出，server 怎么感知？你的代码里具体是哪一行处理的？
- 你遇到过 AI 给的方案编译通过但行为不对的情况吗？怎么发现的？
- 如果让你重新做一遍，你会在哪个环节改变和 AI 的协作方式？
