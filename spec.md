# Lab 1: mini-tmux 规格说明

## 概述

实现一个简化版终端复用器 mini-tmux。mini-tmux 管理多个伪终端（Pseudo-terminal, PTY）窗格（Pane），允许用户在单个终端窗口中同时运行多个 shell 或程序，支持窗格的创建、销毁和焦点切换，支持多个 Client 同时连接。

## 语言与编译

使用 C 或 C++（C11/C++17 或更高）实现，编译产物为单个可执行文件 `mini-tmux`。

项目根目录需提供 `Makefile`，满足：

```bash
make            # 编译生成 ./mini-tmux
make clean      # 清理编译产物
```

不得依赖第三方库（标准库和 POSIX API 除外）。

## 架构要求

mini-tmux 采用 Client-Server 架构：

- **Server 进程**：后台常驻，管理所有 Pane 及其关联的 PTY。负责 fork 子进程、分配 PTY、转发 IO、投递信号。支持多个 Client 同时连接。
- **Client 进程**：前台交互，连接到 Server，显示当前窗格布局并接收用户输入。可以有多个 Client 同时 attach。

Server 与 Client 通过 Unix domain socket 通信。Server 在没有 Client 连接时继续运行，Pane 中的程序不受影响。

## 命令行接口

```
./mini-tmux              # 首次启动：创建 Server + 自动 attach Client
./mini-tmux attach       # 连接到已有 Server（读写模式）
./mini-tmux attach -r    # 连接到已有 Server（只读模式）
```

单一可执行文件。首次运行时，内部 fork 出 Server 进程（后台），然后当前进程作为 Client attach 上去。

Server 实例通过环境变量 `MINI_TMUX_SERVER` 区分。如果该变量已设置，使用其值作为实例名称（用于 socket 路径等）；如果未设置，使用默认名称。这允许同一台机器上运行多个独立的 mini-tmux 实例。

## 多 Client 支持

Server 支持多个 Client 同时 attach 到同一个 session：

- **输出广播**：所有 Pane 的输出同时发送给每个 attached Client
- **输入路由**：只有非只读 Client 可以发送输入。如果有多个读写 Client，所有输入都转发给焦点 Pane（类似 tmux 默认行为）
- **只读 Client**：`attach -r` 的 Client 只接收输出，不能发送按键或命令。Server 忽略只读 Client 的所有输入
- **独立 Detach**：一个 Client detach 不影响其他 Client。Server 只在所有 Client 都断开后进入无 Client 状态
- **终端大小**：当多个 Client attached 时，Server 将 Pane 的 winsize 设置为所有 attached Client 终端大小的最小值（行数取最小，列数取最小）。Client attach 或 detach 时重新计算并更新所有 Pane 的 winsize，发送 SIGWINCH
- **Client 异常断开**：Client 进程被 kill 或连接中断时，Server 不退出，等效于该 Client detach

## 命令模式（Command Mode）

在非只读 Client 中输入以下命令（以冒号开头）操作 Pane：

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

Pane ID 从 0 开始递增编号。首次启动时自动创建 Pane 0。已销毁的 Pane ID 不复用。

## 屏幕布局

当存在多个 Pane 时，Client 必须将终端屏幕分割，同时显示所有 Pane 的内容。

- **布局方式**：垂直等分（每个 Pane 占若干行，Pane 之间用分隔行区分）。不要求水平分割。
- **分隔行**：Pane 之间用一行分隔（内容自定，例如 `--- pane 0 ---`）。分隔行不计入 Pane 的可用行数。
- **Pane 可用行数**：终端总行数减去分隔行数，再平均分配给各 Pane。余数分配给哪个 Pane 不做要求。
- **焦点 Pane**：在分隔行或其他视觉元素上标记当前焦点 Pane（形式不限，能区分即可）。
- **winsize 联动**：每个 Pane 的 PTY winsize 应反映其实际可用行列数（行数为分配到的行数，列数为终端宽度）。Pane 数量变化或终端 resize 时，重新计算布局并更新所有 Pane 的 winsize（发送 SIGWINCH）。

## 焦点切换

有两种切换方式：

1. **命令模式**：`:focus <pane_id>` 直接跳转到指定 Pane
2. **前缀键**：`Ctrl+B`（与 tmux 一致）后按方向键
   - `Ctrl+B` + `n`：切换到下一个 Pane
   - `Ctrl+B` + `p`：切换到上一个 Pane

只有获得焦点的 Pane 接收用户键盘输入。焦点状态在 Server 侧维护，所有 Client 共享同一个焦点 Pane。

## Detach 与 Reattach

- `Ctrl+B` 后按 `d`：当前 Client detach（断开），其他 Client 不受影响，Server 和所有 Pane 继续运行
- `./mini-tmux attach`：新 Client 连接到 Server

详细行为要求：

- Detach 期间 Pane 中的程序正常运行，输出会被 Server 缓存
- Reattach 后 Client 能看到当前焦点 Pane 的屏幕内容
- Client 异常断开（进程被 kill 或连接中断）时，Server 不退出，等效于 detach
- 快速反复 detach/attach 不应导致 fd 或进程泄漏
- Detach 期间 `:log` 和 `:pipeout` 继续工作（Server 侧行为，不依赖 Client）

## :log 输出日志

`:log <pane_id> <file_path>` 将指定 Pane 的 PTY 输出追加写入文件。Server 在收到命令后打开文件（追加模式），后续该 Pane 所有输出同时写入文件。

- `:log-stop <pane_id>` 停止并关闭文件
- Pane 被 `:kill` 时自动停止 log 并关闭文件
- 同一 Pane 重复 `:log` 替换之前的 log 目标（关闭旧文件，打开新文件）
- Detach 期间 log 继续工作

## :pipeout 输出管道

`:pipeout <pane_id> <cmd>` 将指定 Pane 的 PTY 输出实时 pipe 给外部命令。Server fork 子进程执行 `<cmd>`（通过 `/bin/sh -c`），将 PTY master 读到的数据同时写入子进程的 stdin。

- `:pipeout-stop <pane_id>` 手动停止：关闭 pipe 写端，等待子进程退出
- Pane 被 `:kill` 时自动清理 pipe 和子进程
- 外部命令自行退出时自动清理（Server 通过 SIGCHLD/waitpid 感知）
- 同一 Pane 同时只能有一个 pipeout，重复 `:pipeout` 先停止旧的再启动新的
- Detach 期间 pipeout 继续工作

## :capture 屏幕捕获

`:capture <pane_id> <file_path>` 将指定 Pane 当前的屏幕内容导出到文件。Server 将缓存的 Pane 输出历史写入文件（覆盖模式）。

- Server 需要为每个 Pane 维护输出缓冲区（至少保留最近 1000 行）
- 导出的内容应包含最近的输出，不含 ANSI 转义序列（纯文本）
- 多次 `:capture` 同一 Pane 到同一文件，文件内容为最新快照（覆盖）

## PTY 管理要求

每个 Pane 对应一个独立的 PTY Master-Slave 对：

1. **isatty**：Pane 内程序的 stdin 和 stdout 必须是 TTY（`isatty()` 返回 true）
2. **窗口大小**：每个 Pane 的 `winsize` 应反映其在布局中实际分配到的行列数（通过 `TIOCSWINSZ`）。终端 resize、Pane 增减、Client 变化时均需重新计算并更新，发送 `SIGWINCH`。多 Client 时先取所有 Client 终端大小的最小值，再按布局分配
3. **进程组隔离**：每个 Pane 的子进程运行在独立的进程组（Session）中，`tcsetpgrp()` 正确设置前台进程组
4. **信号投递**：`Ctrl+C`（SIGINT）和 `Ctrl+Z`（SIGTSTP）只投递给当前焦点 Pane 的前台进程组，不泄漏到其他 Pane
5. **僵尸回收**：Pane 中的子进程退出后，Server 必须正确 `waitpid()` 回收，不留下僵尸进程（Zombie process）

## 评测方式

评测使用自动化 Harness。Harness 会在你的 Pane 中启动预编译的探针程序（Probe），Probe 通过独立的侧信道（Sideband channel）向 Harness 报告：

- 环境自检结果（isatty, winsize, pid/pgrp）
- 信号接收情况
- IO 数据透传正确性

**你不需要关心 Probe 的实现细节。** 只需确保 Pane 能正确运行任意二进制程序，PTY 管道正确接线即可。

### Probe 的运行方式

Harness 会通过模拟键盘输入，在你的 Pane 的 shell 中执行：

```
/path/to/probe /tmp/sideband.sock session_0
```

这就是一个普通的命令行程序，读 stdin、写 stdout、注册信号 handler。如果你的 PTY 接线正确，Probe 自然能正常工作。

### 评测维度

公开测试用例覆盖以下维度：

- **基础 IO**：单 Pane 环境自检、输入输出 token 透传
- **信号隔离**：多 Pane 下 Ctrl+C/Ctrl+Z 只影响焦点 Pane
- **窗格管理**：创建、销毁、快速创建销毁循环
- **进程管理**：僵尸回收、进程组隔离
- **Resize**：窗口大小变化时正确发送 SIGWINCH 并更新 winsize
- **屏幕布局**：多 Pane 同时可见，winsize 与布局联动
- **压力测试**：8 Pane 并发、高频输出、TUI 程序兼容性
- **会话管理**：Detach/Reattach，Client 异常断开后 Server 存活
- **输出日志**：:log 基础功能
- **输出管道**：:pipeout 基础功能，外部命令退出自动清理
- **多 Client**：多个 Client 同时 attach，输出广播，只读 Client
- **屏幕捕获**：:capture 导出 Pane 内容

### 评分

每个测试用例 Pass/Fail。总分为各用例加权通过率。权重按类别分配：信号隔离和会话管理类权重较高，压力测试类权重较低。

## 运行公开测试

```bash
# 编译你的 mini-tmux
make

# 运行公开测试用例（通过 Docker 黑盒评分）
make grade
```

报告会输出每个用例的通过情况和最终得分。

## 提示

1. 先让单 Pane 的基础 IO 跑通，再逐步增加 Pane 数量
2. 信号隔离是难点，需要正确使用 `setsid()` 和 `tcsetpgrp()`
3. 僵尸回收别忘了，在 Server 中用 `SIGCHLD` handler 或定期 `waitpid(-1, ..., WNOHANG)`
4. Detach/Reattach 的关键是 Server 独立于 Client 的生命周期
5. 多 Client 支持需要 Server 维护 Client 列表，广播输出时遍历所有 connected Client
6. `:pipeout` 需要 Server 额外管理一条 pipe 和子进程，注意清理时机
7. Client 应该将 Pane 的 PTY 输出原样透传到终端，不要过滤或修改
8. `:capture` 需要 Server 维护输出环形缓冲区，剥离 ANSI 转义序列后写入文件
