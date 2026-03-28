# Lab1 快速上手

你大概有过这种经历：SSH 到服务器上跑了个训练脚本，合上笔记本回宿舍，第二天一看，连接断了，进程也没了，什么都没保存。然后有人跟你说“用 tmux 啊”，你试了一下，`tmux` 启动，跑你的程序，`Ctrl+B D` detach，把终端关掉。第二天 `tmux attach`，程序还在跑，输出一行不少。

这是怎么做到的？说实话第一次用 tmux 的时候我也觉得挺神奇的。但你拆开来看，原理并不复杂：你的程序其实从来就没有跑在你的终端里面。tmux 在后台起了一个 Server 进程，你的 bash 和它跑的所有程序都挂在 Server 下面。你看到的那个“终端窗口”只是一个 Client，它做的事情就是把你的键盘输入转发给 Server，再把 Server 传回来的屏幕内容显示出来。所以你关掉终端的时候，死掉的只是这个 Client 进程，Server 和 bash 完全不受影响。

在这个 Lab 里，你要从零构建这整套系统。你造出来的 mini-tmux 应该能做到这些事情：敲一条命令就能进入一个新的会话，在里面跑 bash；按 `Ctrl+B D` 可以断开，但会话不会消失；用 `mini-tmux attach` 能重新连回去；而且你可以同时开好几个终端，各自连着不同的会话。会话里的 shell 退出了，对应的会话就自动销毁。

把这些全做对，基础代码部分就是满分（50 分）。测试用例可能比你想象的刁钻一些，所以 handout 里还有一些 Bonus 功能（最多 25 分），做了可以补基础或报告的缺口。

听起来工作量很大？别被吓到，基础部分的代码量大概在几百行左右。这篇文档会带你一层一层拆开 tmux 的内部结构，前半部分给你能跑的 demo 和详细解释，后半部分越来越少代码、越来越多问题。这是故意的：前面帮你建立直觉，后面让你用这些直觉去指挥 AI。具体的功能 spec 和评分规则都在 [handout.md](handout.md) 里，这里不重复那些内容，我们只聊”怎么想”和”为什么要这样设计”。

## 1 先玩 tmux

习题课上我们已经演示过 tmux 的基本用法了。如果你还没自己动手试过，先装一个玩玩，花两分钟亲手感受一下你要造的东西：

```bash
sudo apt install tmux
tmux
```

你会进入一个看起来像新终端的东西。tmux 把这个东西叫做一个 **session**（会话），你可以把它理解成“一个独立的工作环境，里面跑着自己的 bash”。随便跑点什么（比如 `top`），然后按 `Ctrl+B`，松开，再按 `D`。

终端消失了，你回到了原来的 shell。但 `top` 并没有停，它还在那个 session 里跑着：

```bash
tmux ls
# 0: 1 windows (created ...)  ← 还活着
tmux attach -t 0
# top 还在跑
```

☕️ 多试几件事，建立一下手感：

- 在 tmux 里运行 `sleep 30 && echo done`，detach，等 30 秒后 attach 回来，看看 `done` 有没有出现
- 开两个终端窗口，都 `tmux attach -t 0`，在一个里打字，看看另一个窗口会发生什么
- 多次运行 `tmux`（不带参数），创建几个 session，然后用 `tmux ls` 看看有几个

🌶️ 你关掉了终端窗口，bash 和 top 凭什么还活着？它们到底跑在哪个进程里？（`ps aux | grep tmux` 看看。）

📖 handout 1.1

### 怎么让 AI 帮你

说实话，这个 Lab 的基础部分用 AI 可以不依赖你的任何思考就写完。这篇文档里提的每一个问题，AI 都能给你一个看起来说得过去的回答。所以理解这些问题完全是一个自愿的选择。不过公平地说，进阶和 Bonus 部分是专门设计来防 one-shot 的，AI 在那些地方会开始犯各种微妙的错误，到时候考验的就是你的"驾驶水平"了。而且即使是基础部分，如果你选择去想，你会发现这些问题本身挺有意思的，想明白之后你会获得一种很不一样的能力：打开 AI 生成的几百行代码，你居然能读懂，能看出哪里写对了、哪里有隐患。

这个 Lab 从头到尾都鼓励你用 AI。但”用 AI”不是把题目丢给它然后复制粘贴。大模型最有趣的地方在于它无比”实诚”：你可以骗它，假装你是一个完全不同的人，而它会认真地陪你玩。这在学习的时候非常好用。

比如你刚玩完 tmux，想搞明白它内部是怎么工作的。你可能会这样问：

💬 “我刚体验了 tmux，知道了它怎么用。但我完全不知道 tmux 内部是怎么工作的。请简洁地介绍一下整体原理。”

AI 会给你一段五点概括，提到 server、client、PTY 这些关键词，每个都说得对，但都隔靴搔痒。你读完之后知道的东西并没有比之前多多少。问题出在你的 prompt 太开放了，AI 不知道你卡在哪里。试试把你**具体不懂的地方**说出来：

💬 “你说 client 和 server 通过 socket 通信，但我不知道 socket 是什么。我是 ICS2 的学生，能听懂基本的 OS 术语，请告诉我具体用哪些系统调用。”

这一轮你能拿到系统调用的名字了，但可能还是不知道它们怎么配合。文字解释看三遍也记不住，不如直接要代码：

💬 “我没看懂。请用一段可以编译运行的 C 代码演示 server 和 client 如何建立连接并互相发消息。”

三轮下来你就有了一段能跑的代码。编译，运行，看到 `[server] received: hello from client`，再回去对照 AI 之前说的那些概念，一下就通了。跑一遍代码的效果胜过读十遍文档。

注意这个过程：不是一次问对，而是三轮追问，每轮都比上轮更具体。跟 AI 对话有点像做实验，关键不在于第一个 prompt 写得多完美，而在于你能不能根据”哪里没懂”精准地追问。会追问的人，三轮对话就能把一个陌生概念搞透；不会追问的人，问十次拿到的都是泛泛而谈。

⚠️ AI 也会犯错，而且在这个 Lab 里犯得还挺有规律。举个例子：你问 AI”tmux 里为什么会出现双重回显”，它大概率回答”因为终端默认会回显输入”。这个回答不能说错，但它没告诉你关键信息：是**哪个**终端在回显？有几个 TTY 驱动？该关哪个？如果你没有自己想清楚这个问题（第 4 节会展开），你就没法判断 AI 的回答是否到位，最后代码跑起来出了 bug 也不知道从何查起。再比如 fd 泄漏的问题（第 5 节会讲），你问 AI”fork 之后需要注意什么”，它可能列出一堆注意事项但偏偏漏掉”关闭继承来的 slave_fd”，因为这个细节需要理解 PTY 的 EOF 机制才能想到。AI 最容易在这种”需要串联多个概念”的地方犯错。不要无脑复制粘贴，跑一遍测试，出了问题再问 AI”为什么我的程序在 XXX 情况下 hang 住了”，它反而能给你很好的诊断。

## 2 单 Pane：50 行代码造一个”终端”

好，现在你知道 tmux 能做什么了。接下来我们从最简单的一步开始：先不管 Client-Server，先不管 detach，先搞明白一件事，怎么在你的程序里跑一个 bash，让这个 bash 以为自己在一个真实的终端里。

`demos/pty_shell.c` 是一个只有 50 行的小程序，先编译跑一下：

```bash
cd demos
gcc -o pty_shell pty_shell.c -lutil
./pty_shell
```

你会得到一个 shell 提示符。试试 `ls`、`vim`、`top`，你会发现颜色、光标移动、tab 补全全都正常，看起来和普通终端没什么区别。

但它不是普通终端。在里面运行 `tty`，看看输出是什么，然后再开一个普通终端运行 `tty`，对比一下。

🌶️ 为什么不一样？`/dev/pts/X` 到底是个什么东西？

退出（`Ctrl+D`），打开 `pty_shell.c` 的源码读一读。50 行代码，核心就三步：

1. `forkpty()` 创建一对 PTY（伪终端），顺便 fork 出一个子进程
2. 子进程在 PTY 的 slave 端执行 `exec bash`，bash 以为自己连着一个真实终端
3. 父进程用 `poll()` 在键盘和 PTY 的 master 端之间来回搬运数据

```
pty_shell 做的事：

  键盘 → 父进程 → master_fd ···PTY··· slave_fd → bash
  屏幕 ← 父进程 ← master_fd ···PTY··· slave_fd ← bash
```

这就是 tmux 最底层的机制：用一个 PTY 把 bash“骗”进一个虚拟的终端里，然后你的程序在 master 端操控它。

### PTY 是什么

PTY（Pseudo-Terminal，伪终端）本质上就是一对互相连通的字符设备：你往 master 端写的数据会从 slave 端读出来，反过来也一样。

你可以把它想成一面**双面镜**。bash 站在 slave 那一侧，它往镜子里一看，看到的是一个完全正常的终端设备：`isatty()` 返回 true，行编辑能用，颜色代码能用，光标控制序列也正常。但镜子的另一面，也就是 master 端，谁拿着 master 端的 fd，谁就能看到 bash 写给”终端”的所有输出，也能往里面注入任何输入。bash 完全不知道自己面对的不是真实的硬件终端，这也正是 tmux 能工作的基础。

那如果不用 PTY，改用普通的 pipe 连接 bash 会怎样？bash 会发现“镜子不见了”，面前只有一根管子。它检测到自己不在终端里（`isatty()` 返回 false），就会关掉交互模式，退化成批处理模式。你可以亲手试试：把 `pty_shell.c` 里的 `forkpty` 换成 `fork` + `pipe`，然后在里面跑 `vim`，看看会发生什么（相信我，效果挺有意思的）。

创建 PTY 有两种方式。`forkpty()` 帮你一步搞定 fork + 创建 PTY，用起来很方便。注意 `forkpty()` 不会调用 `setsid()`，后面写 Server 的时候你需要自己调（见 handout 3.3）。另一种是自己调 `openpty()` 或者 `posix_openpt()` + `grantpt()` + `unlockpt()` 手动创建，麻烦一些，但能控制更多细节（比如 PTY 的 termios 属性）。你后面写 Server 的时候可能会需要后者，到时候再说。

📖 handout 2.1（PTY）、3.3（单 Pane 基础）

### 但这还不是 tmux

`pty_shell` 跑起来虽然很像一个终端，但它有一个根本的问题：你关掉终端窗口，父进程就死了，master_fd 被关闭，PTY 断开，bash 也跟着退出。没办法 detach，没办法 reattach，和直接开一个终端跑 bash 没有本质区别。

问题出在哪？PTY 的 master 端和你的终端窗口绑死在同一个进程里了。打个比方，你在一个录音棚里给隔壁房间的歌手（bash）当调音师，你坐在调音台前（master 端），通过一面玻璃窗（PTY）看着歌手，帮他调音量、播伴奏。但现在调音台和你坐的椅子是焊在一起的，你一起身离开，调音台就断电了，歌手也被迫停下来。

怎么解决？把调音台和椅子拆开。让调音台接上独立电源自己运行（这就是 Server 要做的事），你的椅子变成一个无线遥控器（这就是 Client）。遥控器没电了、你把它带走了都无所谓，调音台照常工作，歌手继续唱。什么时候你拿来一个新遥控器连上，又可以继续操控了。

这也是为什么 tmux 对 SSH 用户特别有用。你通过 SSH 连到远程服务器，SSH 连接本质上就是你的“遥控器”。如果你不用 tmux，SSH 断了（网络抖动、笔记本合盖、WiFi 切换），你跑的程序就跟着死了。但如果你在 tmux 里跑，SSH 断了只是 Client 没了，Server 还在服务器上活着。下次 SSH 重新连上，`tmux attach`，一切还在。当然，如果服务器本身关机了，那 Server 进程也没了，这个谁都救不了。

## 3 Client-Server：拆成两个进程

这是整个 Lab 最关键的一步。一旦你把“管理 PTY 和 bash”跟“显示屏幕、读键盘”拆到两个独立的进程里，detach 的能力就自然出现了：

```
pty_shell（一个进程干所有事）：

  键盘 → [父进程] → master_fd ··· bash
  屏幕 ← [父进程] ← master_fd ··· bash


拆成 Client + Server：

  键盘 → [Client] ══socket══ [Server] → master_fd ··· bash
  屏幕 ← [Client] ══socket══ [Server] ← master_fd ··· bash

  关掉终端？Client 死了。
  Server 和 bash 还在。
  开个新 Client 连上来就行。
```

那 Client 和 Server 之间用什么通信呢？答案是 Unix domain socket。

### Unix domain socket 是什么

你的第一反应可能是用 pipe，但 pipe 有个限制：它是在 `fork()` 的时候通过文件描述符继承传递的，只能在有亲缘关系的进程之间使用。tmux 的使用场景不是这样的：Server 可能早就启动了，过了半天，一个全新的 Client 进程才出现，说“我要连上来”。这种“先开店，客人随时来”的模式，pipe 做不到。

Unix domain socket 就是为这种场景设计的。简单来说，它就是同一台机器上两个进程之间的双向管道，但这个管道**有一个名字**。Server 在文件系统上创建一个 socket 文件（比如 `/tmp/mini-tmux.sock`），之后任何进程只要知道这个路径，就可以通过 `connect()` 连上来跟 Server 通信。而且一个 Server 能同时接待好多个 Client，这一点后面做多 Client 的时候会用到。

你可能已经注意到了：socket 文件出现在文件系统里，连上之后用 `read()` / `write()` 通信，返回的也是文件描述符。这就是 Unix 的核心哲学：everything is a file。普通文件是 fd，pipe 是 fd，终端是 fd，PTY 是 fd，socket 也是 fd。你已经会用 `read()` / `write()` 读写文件了，那你就已经会跟 socket 通信了。整个 mini-tmux 从头到尾，你打交道的就是各种 fd 之间的数据搬运。

### 跑一个例子

光看概念可能有点抽象。`demos/echo_server.c` 和 `demos/echo_client.c` 是一个最小的 socket 通信示例，加起来大概 70 行。先编译跑一下看看效果：

```bash
gcc -o echo_server echo_server.c
gcc -o echo_client echo_client.c

# 终端1
./echo_server
# [server] 等待 client 连接...

# 终端2
./echo_client
# [client] 已连接
# [client] 发送: hello from client
# [client] 收到回复: hello from client
```

Server 收到 Client 发来的消息，原样回复，然后两边都退出。代码不长，建议仔细读一遍，后面你自己写 mini-tmux 的通信部分基本就是在这个骨架上扩展。

### 5 个系统调用

读这两个文件的时候你会碰到 5 个 socket 相关的系统调用。别被数量吓到，它们之间的关系其实非常机械，就像打电话有固定步骤一样。而且这套 `socket` / `bind` / `listen` / `accept` / `connect` 的流程不是 Unix domain socket 专用的，网络编程里的 TCP 通信用的也是同一套接口，只是把 `AF_UNIX` 换成 `AF_INET`、把文件路径换成 IP 地址和端口号。这门课后面讲网络的时候你还会再见到它们，所以现在搞明白了等于提前学了：

```
Server 端：                       Client 端：

socket()  — 创建通信端点           socket()  — 同样
   ↓                                 ↓
bind()    — 绑定一个地址           connect() — 连到 server 的地址
   ↓                                 ↓
listen()  — 开始等待连接           read() / write()
   ↓        （立刻返回，内核在后台排队）
accept()  — 接住一个 client
   ↓         （返回一个新 fd，
              专门和这个 client 通信）
read() / write()
```

`bind()` 绑定的“地址”在 Unix domain socket 里就是一个文件路径（比如 `/tmp/mini-tmux.sock`）。Client 通过 `connect()` 连到同一个路径，连上之后双方就可以 `read()` / `write()`，用法和读写普通文件差不多。

这里有一个容易忽略但很重要的细节：`accept()` 返回的是一个**全新的 fd**，不是原来那个 listen 用的 fd。原来那个 fd 继续留着等下一个连接进来，新返回的 fd 则专门负责跟这一个 Client 通信。你可以把 listen_fd 想成酒店前台的总机号码，每个客人打进来之后，前台给你转接到一部分机上（conn_fd），总机本身不占线，随时可以接下一个客人。这就是为什么一个 Server 能同时服务多个 Client。

### Server 是怎么启动的

你运行 `tmux` 的时候只敲了一个命令，但背后可能起了两个进程。实际的流程大概是这样的：

1. `tmux` 进程启动，先尝试 `connect()` 到一个约定好的 socket 路径（比如 `/tmp/tmux-xxx/default`）
2. 如果连上了，说明 Server 已经在跑了，那当前进程就直接当 Client 用
3. 如果连不上（socket 文件不存在，或者没有进程在 listen），说明 Server 还没启动
4. 于是 `fork()` 一下：子进程变成 Server（负责 `bind` + `listen`），父进程继续当 Client
5. Client 再尝试 `connect()`，这次 Server 已经起来了，能连上了
6. Server 那边 `accept()`，双方建立连接，开始正常通信

```
tmux 启动流程：

  tmux 进程启动
       │
       ├─ connect() 成功？──── 是 ──→ 当 client，结束
       │
       └─ connect() 失败 ──→ fork()
                                ├─ 子进程：bind + listen（变成 server）
                                └─ 父进程：再次 connect()（当 client）
```

你的 mini-tmux 不一定要完全照搬这个流程，但最终的编译产物必须是单个可执行文件 `mini-tmux`（见 handout 1.3）。开发过程中可以先用简单方案跑通，但最终要让用户只敲一条命令就能用起来。

📖 handout 2.4（Unix domain socket）、2.5（I/O 多路复用）、3.1（架构）

### 通信协议：Client 和 Server 之间传什么

C/S 的连接骨架搭好了，两边能互相发字节了。但具体发什么、用什么格式发？这是你需要自己设计的部分，没有标准答案。不过你至少得想清楚下面这些事情：

**Client → Server 需要传的东西**：
- 用户的键盘输入，每按一个键就要传过去（包括 Ctrl 组合键）
- 控制指令：创建新 session、detach、attach 到指定 session、kill session，等等

**Server → Client 需要传的东西**：
- PTY 的输出，也就是 bash 打印的内容、vim 画的界面之类的
- 状态信息：当前有哪些 session、你 attach 在哪个上面，等等

🌶️ 键盘输入和控制指令怎么区分？当你按 `Ctrl+B` 然后按 `D` 的时候，这应该被理解为一个 detach 指令。但 `Ctrl+B` 本身就是一个普通字节（0x02），跟其他输入混在一起传过来。Server 怎么知道这是指令前缀而不是你想往 bash 里打一个 `Ctrl+B`？这个问题值得好好想想。

🌶️ 数据格式也值得考虑。最简单的方案是直接传原始字节流，但这样你很难区分不同类型的数据。另一种方案是给每条消息定义一个消息头（类型 + 长度 + 内容），但你就得处理“消息被拆成多次 `read()` 才收完”的情况。两种方案各有什么坑？

这个协议设计是整个 Lab 里我觉得最值得自己思考的部分。让 AI 帮你写具体的代码完全没问题，但协议本身建议先自己想清楚，因为它决定了后面所有代码的骨架。协议定错了后面推倒重来的代价会很大，花点时间在纸上画画是值得的。

## 4 Client 要解决的三个问题

到这里，架构上最难的部分其实已经过去了：你知道 PTY 是什么，知道 C/S 怎么拆，知道通信用什么。接下来就是把想法变成能跑的代码。

协议定好了，可以动手写 Client 了。Client 只负责一件事：把键盘输入搬到 socket，把 socket 数据搬到屏幕。它不管 PTY，不管 bash，不管 session，就是一个双向搬运工。

```
Client 内部：

  键盘 (stdin)                     屏幕 (stdout)
      │                                ▲
      ▼                                │
  ┌────────────────────────────────────────┐
  │  Client 进程                           │
  │                                        │
  │  raw mode：关掉行缓冲和回显            │
  │  poll()：同时盯着 stdin 和 socket_fd   │
  └────────────────────────────────────────┘
      │                                ▲
      ▼                                │
   socket_fd ══════════════════ socket_fd
      │                                │
      ▼                                │
         ↓↓↓ 这里往下就是 Server 的世界了 ↓↓↓
```

这是前面那张大图的上半部分。下半部分（Server 那边怎么接住这些数据、怎么转给 bash）在第 5 节会展开。

但你动手的时候马上会撞上三个具体的技术问题。

这三个问题都跟终端（TTY）有关。在 Linux 里，终端不是一个普通的文件。前面说过 everything is a file，终端确实也是一个 fd，你可以对它 `read()` / `write()`。但终端的特殊之处在于，它和你的程序之间还隔着一层内核里的 TTY 驱动（也叫线路规程，line discipline）。你打开 iTerm、gnome-terminal 或者任何终端模拟器的时候，内核都会给它分配一个 PTY，这个 PTY 里就有一个 TTY 驱动。所以你的 Client 进程从 stdin 读输入、往 stdout 写输出的时候，中间都要经过这层驱动。注意，在 mini-tmux 里有**多个** PTY：Client 这边有一个（你的终端模拟器背后的），Server 那边每个 Session 各有一个（bash 跑在里面的）。单 Session 的时候就是两个，多 Session 就是 1 + N 个。这一节先说 Client 这边的，Server 那边的第 5 节会讲到。Client 这边的 TTY 驱动会“好心地”帮你做很多事情：攒够一行再交给程序（行缓冲）、把你打的字自动显示出来（回显）、看到 `Ctrl+C` 就发信号杀进程……在普通程序里这些默认行为很方便，但在写 tmux Client 的时候，它们每一个都会给你添麻烦。

### 问题一：怎么不等回车就读到按键

你以前写的 C 程序里，`scanf()` 和 `getchar()` 都是要等你按了回车才返回的。这是因为终端默认工作在 canonical 模式（也叫行缓冲模式）：你打的字先攒在内核的缓冲区里，按下回车之后才一次性交给程序。

但 Client 不能这样工作。你按下 `Ctrl+B` 的那一刻，Client 就要立刻拿到这个按键（因为这可能是 detach 指令的前缀），如果等到用户按回车才知道就太晚了。

解决办法是把终端切到 raw 模式：用 `tcgetattr()` 保存当前的终端设置，调 `cfmakeraw()` 切换成 raw 模式，程序退出的时候再用 `tcsetattr()` 恢复回去。

```bash
# 在终端里试试这个命令，感受一下终端的配置项
stty -a
# 你会看到 icanon 和 echo 这两个标志
# icanon = 行缓冲模式开启
# echo = 你打的字自动显示到屏幕上
# raw 模式就是把这些全关掉
```

⚠️ 如果你的 Client 崩溃了（比如段错误），没来得及恢复 termios 设置，终端就会一直停在 raw 模式。这时候你打字看不到任何东西，回车也不换行，看起来像终端彻底挂了一样。别慌，这个我们都经历过，盲打 `reset` 然后按回车就能恢复正常。

### 问题二：怎么关掉自动回显

先说一下正常情况下终端是怎么工作的。你打开一个终端，敲一个 `a`，屏幕上马上就出现一个 `a`。这个 `a` 是谁显示的？不是你的程序，是终端驱动（TTY driver）。在你的程序还没来得及读到这个 `a` 之前，内核里的终端驱动就已经帮你把它“回显”（echo）到屏幕上了。这是传统终端的默认行为，对普通程序来说很方便，你不需要自己操心显示。

但在 tmux 的场景里这就成了问题。屏幕上显示什么应该完全由 Server 那边的程序决定，Client 自己不能擅自显示任何东西。为什么？因为 Client 不知道按键对应什么屏幕变化：你按 `l`，如果里面跑的是 bash，该显示一个 `l`；但如果跑的是 vim，该做的是光标下移，屏幕上不应该出现 `l`；如果跑的是 `sudo`，你在输密码，什么都不该显示。只有 Server 那边的程序知道正确的响应是什么。所以正确的做法是：Client 把按键发给 Server，Server 那边的程序处理完之后通过 PTY 输出回来，Server 再推给 Client 显示。如果 Client 这边的终端驱动同时也自作主张回显了一遍，屏幕上就会多出来一份。

`cfmakeraw()` 在切换 raw 模式的时候会顺便把 echo 也关掉，所以你不需要额外处理。但你要理解这意味着什么：关掉 echo 之后，你的 Client 就完全接管了“屏幕上显示什么”这件事。如果 Server 挂了或者 socket 断了，你在键盘上打的字不会出现在屏幕上，因为终端驱动已经不帮你干这件事了。第一次遇到这种情况可能会觉得程序卡住了，其实只是没有人在回显而已。

☕️ 其实不只是你的 Client 要关回显。所有需要自己控制屏幕内容的交互式程序都会这么做：vim、top、less、ssh，它们启动时都会把终端切成 raw + no echo，退出时再恢复。shell 自己也是，readline 库需要拿到每一个按键才能实现 tab 补全和方向键（比如你在 zsh 里按上箭头能翻出上一条历史命令，这就是 shell 自己在 raw 模式下处理按键实现的），所以 shell 也工作在 raw + no echo 下。你的 Client 和它们的需求是一样的。可以回头看看 handout 2.6 里 canonical 和 raw 模式的对比。

### 问题三：怎么同时从两个 fd 读数据

Client 需要同时盯着两个地方：

- stdin，也就是用户的键盘输入，读到了要转发给 Server
- socket fd，也就是 Server 推过来的屏幕数据，读到了要写到终端上显示

你没有学过多线程，也不需要用多线程来解决这个问题。其实 `pty_shell.c` 里已经演示过答案了：`poll()`。这个技术叫 I/O multiplexing（I/O 多路复用），名字听起来吓人，其实就是“一个线程同时盯着好几个 fd，谁有数据就处理谁”。tmux 名字里的 mux 就是 multiplexer 的缩写，终端多路复用器。把两个 fd 放进一个 `pollfd` 数组，调用 `poll()` 等着，哪个 fd 有数据进来就处理哪个，两个都没数据的时候就阻塞等：

```c
struct pollfd fds[2] = {
    { .fd = STDIN_FILENO, .events = POLLIN },
    { .fd = server_fd,    .events = POLLIN },
};

while (1) {
    poll(fds, 2, -1);

    if (fds[0].revents & POLLIN) {
        // 键盘有输入 → 读出来，发给 server
    }
    if (fds[1].revents & POLLIN) {
        // server 有数据 → 读出来，写到屏幕
    }
}
```

这三个问题解决了，Client 基本就写完了。你回头看看会发现 Client 做的事情其实非常少：它就是一个 raw 模式下的双向数据搬运工，从键盘搬到 socket，从 socket 搬到屏幕。真正复杂的逻辑全在 Server 那边。

📖 handout 2.6（raw mode / termios）、2.5（poll / epoll）

## 5 Server 要做什么

Client 写完了？恭喜，整个 Lab 里最轻松的部分结束了。接下来是 Server，它才是真正干活的那个。Server 负责的是“真正干活”的部分：管理所有的 PTY 和 bash 子进程，接受 Client 的连接，把键盘输入转给对应的 bash，把 bash 的输出推给对应的 Client。听起来很多，但别被吓到。你拆开来看的话，Server 的骨架其实也是一个 `poll()` 事件循环，和 Client 的结构是类似的，只不过同时监听的 fd 更多：

- listen_fd：有新 Client 要连接进来
- 每个已连接 Client 的 socket fd：Client 发来了键盘输入或者控制指令
- 每个 session 的 PTY master_fd：bash 那边有输出了

有数据进来就处理，处理完回到 `poll()` 继续等。所有的“同时管好几件事”都是通过这一个循环实现的。

```
Server 内部：

  Client 0   Client 1   ...
  socket_fd  socket_fd
      │          │
      ▼          ▼
  ┌──────────────────────────────────────────────────────────────┐
  │  Server 进程                                                 │
  │                                                              │
  │  poll()：同时盯着 listen_fd + 所有 client_fd + 所有 master_fd│
  │                                                              │
  │  client_fd 有数据 → 写到对应 session 的 master_fd            │
  │  master_fd 有数据 → 推给 attach 到这个 session 的 client_fd  │
  │  listen_fd 有数据 → accept() 新 Client                      │
  │                                                              │
  │          Session 0                    Session 1              │
  │   master_fd ···PTY··· slave_fd  master_fd ···PTY··· slave_fd│
  │                          │                             │     │
  │                         bash                          bash   │
  └──────────────────────────────────────────────────────────────┘
```

### 走一遍完整的数据流

抽象地说不如举个具体例子。假设你在 Client 里敲了 `ls` 然后按回车，从按键到屏幕上出现文件列表，中间发生了什么？先回忆一下：每个 session 有一对 PTY，Server 拿着 master_fd，bash 拿着 slave_fd（就是 bash 的 stdin/stdout/stderr）。

```
1. 你按下 'l'
   键盘 → Client 的 stdin

2. Client 从 stdin 读到 'l'，通过 socket 发给 Server
   Client → socket → Server

3. Server 查表：这个 Client attach 在 session 0 上
   Server 把 'l' 写到 session 0 的 master_fd

4. PTY 把 'l' 传给 bash（通过 slave_fd）
   master_fd → PTY → slave_fd → bash

5. bash 把 'l' 回显出来，经过 PTY 传回 master_fd
   slave_fd → PTY → master_fd

6. Server 从 master_fd 读到这个 'l'
   Server 查表：谁 attach 在 session 0 上？→ 这个 Client
   Server 通过 socket 把 'l' 推给 Client

7. Client 从 socket 读到 'l'，写到 stdout
   屏幕上出现 'l'
```

按 's'、按回车也是同样的流程。按回车之后 bash 执行 `ls`，输出一堆文件名，这些文件名走的也是 5→6→7 这条路径回到你的屏幕。

你可能注意到了第 5 步：bash 为什么会回显？因为 Server 那边的 PTY 里有一个 TTY 驱动（第 4 节开头提到的两个 TTY 驱动之一），bash 的输入经过这个驱动的时候会被自动回显到 master_fd。

现在你也能理解第 4 节为什么要关 Client 的 echo 了。两个 PTY 都有 TTY 驱动，如果都开着 echo，按一次 `l` 就会出现两份：

```
你按 'l'
   │
   ▼
Client 的 PTY（TTY 驱动）
   ├─ echo 开着？→ 直接显示 'l' 到屏幕  ← 第一份（要关掉的）
   │
   ▼
Client 进程读到 'l' → socket → Server → master_fd
                                           │
                                    Server 的 PTY（TTY 驱动）
                                           ├─ echo → 回显 'l' 到 master_fd
                                           ▼
                                    Server 读到 'l' → socket → Client → 屏幕  ← 第二份（要保留的）
```

`cfmakeraw()` 关的是上面那个分叉，让 Client 的 TTY 驱动不要自作主张，屏幕上只留 bash 传回来的那一份。

所以 Server 的核心工作就是两个方向的搬运：

- **client_fd → master_fd**：Client 发来按键，转给对应 session 的 bash
- **master_fd → client_fd**：bash 有输出，推给 attach 在这个 session 上的所有 Client

除此之外，Server 还要处理 Client 发来的控制指令：创建 session、attach、detach、列出 session 等等。

📖 handout 3.2（命令行接口）、3.11（命令汇总表）

### Server 的 PTY 管理

每创建一个新 session，Server 都要创建一对 PTY 并 fork 一个子进程来跑 bash。这和第 2 节 `pty_shell.c` 里做的事情是一样的，但 Server 里需要多操心几件事：

你 fork 出来的子进程需要调用 `setsid()` 建立一个自己的**进程会话**（process session）。注意这里说的“会话”是 Linux 内核里的概念，和 tmux 的 session 不是一回事，只是恰好同名。Linux 的进程会话决定了信号发给谁：比如 `Ctrl+C` 产生的 SIGINT 会发给当前会话的前台进程组。如果你的 bash 子进程不建立自己的进程会话，多个 tmux session 里的 `Ctrl+C` 信号就会串在一起（后面信号隔离那节会详细讲）。`setsid()` 之后还需要设置控制终端，具体的步骤这里就不展开了。

另外，fork 出来的子进程会继承 Server 的所有 fd，包括 listen_fd、所有 client 的 socket fd、其他 session 的 master_fd。这些 fd 对子进程来说完全没用，但如果你忘记关掉它们，会出一个很隐蔽的 bug。

举个例子：你有 session 0 和 session 1。bash 退出的时候会关闭它持有的 slave_fd，当一个 PTY 的 slave 端所有 fd 都关了，从 master 端 `read()` 就会返回 0（EOF），Server 靠这个来判断“这个 session 的 bash 死了”。但如果 session 1 的子进程继承了 session 0 的 slave_fd 没关掉，那 session 0 的 bash 退出后 slave 端的引用计数还不是零，master 端就读不到 EOF。结果 Server 永远不知道 session 0 的 bash 已经死了，这个 session 变成了僵尸。

解决办法很简单：fork 之后在子进程里把所有不需要的 fd 全部 close 掉。这种 fd 泄漏是这个 Lab 里最常见的 bug 之一，提前知道能省很多 debug 时间。

🌶️ 你可以用 `strace -f` 跟踪一下真正的 tmux 在启动的时候做了些什么：
```bash
strace -f -e trace=openat,ioctl,clone,setsid tmux
```
你会看到 `openat("/dev/ptmx")` 创建 PTY，`clone()` fork 子进程，子进程里 `setsid()` 建立新会话，`ioctl(TIOCSCTTY)` 设置控制终端，然后一堆 `close()` 关掉继承来的多余 fd。这就是上面说的那些步骤在真实系统里的样子。

📖 handout 2.2（进程 进程组与会话）、2.7（文件描述符 dup2 与 pipe）

### Server 的僵尸回收

bash 退出的时候，因为 Server 是它的父进程，内核会给 Server 发一个 SIGCHLD 信号。Server 必须调用 `waitpid()` 来回收这个子进程，否则它会变成僵尸进程（zombie），一直占着进程表里的一个位置。

通常的做法是注册一个 SIGCHLD 的 signal handler，在里面用 `waitpid(-1, &status, WNOHANG)` 循环回收所有已经退出的子进程。用 `WNOHANG` 是因为可能同时有多个子进程退出，你需要一次全部回收干净，不能只回收一个就走了。

📖 handout 3.10（进程管理与资源清理）

## 6 Detach / Reattach

如果你前面的 Client 和 Server 都写对了，detach 和 reattach 几乎不需要写多少额外的代码，因为这个能力本来就是 C/S 分离带来的。你走到这一步的时候应该会有一种“哦，这不就自然而然的嘛”的感觉。

**Detach 的时候发生了什么？** Client 断开了 socket 连接（可能是用户主动发了 detach 指令，也可能是直接关掉了终端窗口）。Server 那边会发现某个 client_fd 变得可读，但 `read()` 返回了 0（EOF），这意味着这个 Client 走了。Server 把这个 client_fd 从 poll 集合里移除，清理掉相关的状态，但 session 和 PTY 原封不动地留着，bash 继续跑。

**Reattach 呢？** 用户在一个新终端里启动 Client，Client 通过 `connect()` 连到 Server 的 socket。Server `accept()` 得到一个新的 client_fd。Client 告诉 Server“我要 attach 到 session 0”，Server 就把 session 0 当前的屏幕内容推给它，然后正常转发数据。对 Server 来说，新来的 Client 和之前那个没有任何区别。

🌶️ 说到“把屏幕内容推给新 Client”，这里有个有意思的问题：Server 怎么知道 session 0 的屏幕上现在长什么样？bash 之前输出的那些内容已经从 master_fd 读走了，不会再出现第二次。Server 需要自己在内存里维护一份屏幕缓冲区吗？还是有别的办法？这个问题值得好好想想。

🌶️ 如果两个 Client 同时 attach 到同一个 session，Server 应该怎么处理？两个人都能打字吗？bash 的输出推给谁？

⚠️ 这里有个很容易踩的坑：Server 往一个已经断开的 socket 里 `write()` 会触发 SIGPIPE 信号，而 SIGPIPE 的默认处理方式是直接杀死进程。也就是说，如果你不处理这个信号，一个 Client 断开连接就有可能把整个 Server 干掉。所以你的 Server 在启动的时候一定要加上 `signal(SIGPIPE, SIG_IGN)`。提前加上这一行能省你不少 debug 时间。

📖 handout 3.7（Detach 与 Reattach）

☕️ 如果你走到这一步，试试打开你的 Server 代码通读一遍。你可能会发现一件有意思的事情：虽然大部分代码是 AI 写的，但你居然能读懂。你知道这个 `poll()` 在盯着哪些 fd，这个 `write()` 在往哪个方向搬数据，这个 `close()` 是在防 fd 泄漏。不是每一行都清楚，但骨架你是明白的。这就是前面那些问题的回报。

## 7 多 Session / 多 Client

前面你的 Server 只管一个 session，现在要扩展成一个 Server 管多个 session。用户可以通过命令创建新 session、列出所有 session、attach 到指定 session。

你可能会想：为什么不给每个 session 单独跑一个 Server？因为如果 session 分散在不同进程里，将来想做 session 之间的快速切换就很麻烦（基础部分还不需要切换，但架构上提前留好余地是值得的），也没法让多个 Client 灵活地连到不同的 session 上。一个 Server 统一管理所有 session，这正是 multiplexer 的意思。

所以最终的结构是：一个 Server 同时管理多个 session，每个 session 有自己独立的 PTY 和 bash 进程。多个 Client 可以同时连到 Server，各自 attach 到不同的 session（也可以多个 Client attach 到同一个 session，见 handout 3.8）。Client 可以 detach 之后重新 attach 到不同的 session。

Server 的 `poll()` 现在要盯的 fd 变多了：listen_fd、当前连着的 client_fd（如果有的话）、每个 session 的 master_fd。你需要一个数据结构来记录当前 Client attach 在哪个 session 上，这样 Client 发来键盘输入的时候你才知道该写到哪个 master_fd 里。

### 信号隔离

这里有一个设计上很重要的问题，也是很容易忽略的。用户在 session A 里按了 `Ctrl+C`，这个 `0x03` 字节从 Client 经过 socket 到达 Server，Server 把它写入 session A 的 PTY master_fd。PTY 的线路规程（line discipline）看到 `0x03`，发现当前 slave 的 termios 开启了 ISIG，于是向 slave 端的前台进程组发送 SIGINT。

关键在于，这一切都只发生在 session A 的 PTY 内部。session B 有自己独立的一对 PTY，完全不受影响。这就是为什么每个 session 的 bash 必须跑在自己的 PTY 里，有自己的 session leader，有自己的前台进程组。如果你图省事让多个 bash 共享一个 PTY，那用户在任何一个 session 里按 `Ctrl+C` 都会杀掉所有 session 的前台进程，那就不叫“多 session”了。

📖 handout 2.2（进程组与会话）、2.3（信号）、3.6（信号投递）

## 完成这四个阶段 = 代码部分满分

| 阶段 | Demo | 做完的标志 |
|------|------|-----------|
| 单 Pane | `demos/pty_shell.c` | vim 能正常用，`Ctrl+C` 不杀你的程序 |
| Client-Server | `demos/echo_server.c` + `echo_client.c` | 拆成两个进程，效果和单 Pane 一样 |
| Detach/Reattach | — | Client 断开后 Server 还在，新 Client 能连回来 |
| 多 Session/Client | — | 多 session 各自独立，`Ctrl+C` 不跨 session |

Bonus（多 Pane、Layout、SIGWINCH、Log、Pipeout、Capture）见 handout 3.4-3.5、3.9。

## 接下来

打开 [handout.md](handout.md)，第 2 节是前置知识速查，第 3 节是完整的功能 spec，边做边查。

另外 `workloads/public/` 下面的 YAML 文件描述了每个测试用例的具体步骤和验证条件。这些文件不是可执行的脚本，而是测试的“剧本”：每一步做什么、验证什么，写得很清楚。有时候读测试文件比读文档还直接。举个例子，`01_single_pane_basic.yaml` 长这样：

```yaml
name: "Single pane basic IO"
timeout_sec: 15
terminal:
  rows: 24
  cols: 80
steps:
  - action: start                  # 启动你的 mini-tmux
  - action: create_pane_with_probe # 创建一个 pane，里面跑 probe（测试用的小程序）
  - action: wait_probe_ready       # 等 probe 启动完成
    pane: 0
  - action: verify_env_check       # 验证 probe 的环境自检通过（isatty、termios 等）
    pane: 0
  - action: wait
    duration_ms: 1000
  - action: verify_output_token    # 验证 probe 输出的 token 能到达 client
    pane: 0
  - action: verify_input_token     # 验证 client 发的 token 能到达 probe
    pane: 0
```

从上往下读就行：先启动，创建 pane，等 probe 就绪，然后验证环境、输出、输入。每个 `verify_` 开头的步骤就是一个断言，过了就得分。再看一个 detach 的测试 `13_detach_reattach_basic.yaml`：

```yaml
steps:
  - action: start
  - action: create_pane_with_probe
  - action: wait_probe_ready
    pane: 0
  - action: verify_env_check
    pane: 0
  - action: detach                 # 断开 client
  - action: verify_server_alive    # 验证 server 还活着
  - action: reattach               # 重新连接
  - action: wait
    duration_ms: 500
  - action: verify_input_token     # 验证重连后 IO 还能正常工作
    pane: 0
```

detach 之后验证 server 没死，reattach 之后验证 IO 还通，逻辑一目了然。建议你每做完一个阶段就去读对应的 YAML 文件，搞清楚评测在检查什么，比盲猜有用得多。有问题随时在群里问。

## 最后

你可能会觉得这个 Lab 有点奇怪：大部分代码是 AI 写的，我自己到底做了什么？

### Prompt = 源代码，LLM = 编译器

换个角度想。你用 C 写代码的时候，其实也没有在”写机器码”，你写的是一份对逻辑的描述，然后 gcc 帮你把它翻译成机器能执行的二进制。你不会因此觉得”代码是 gcc 写的，不是我写的”，对吧？因为真正重要的东西，程序的结构、模块的分工、边界情况的处理，都是你决定的。gcc 只是一个翻译器。

现在把 gcc 换成 LLM。你想明白了 Client 和 Server 应该怎么分工，想明白了协议长什么样，想明白了 PTY 要怎么管理、detach 的时候状态怎么保留。然后你把这些想法写成 Prompt，LLM 帮你翻译成了 C 代码。从这个角度看，Prompt 就是你的”源代码”，LLM 就是你的”编译器”。编译器变强了，不代表程序员变弱了，只是你描述意图的层次变高了。

```
    你的理解                         你写的代码
        │                               │
        ▼                               ▼
  ┌───────────┐                   ┌───────────┐
  │  Prompt   │                   │   .c 文件  │
  └─────┬─────┘                   └─────┬─────┘
        │                               │
        ▼                               ▼
  ┌───────────┐                   ┌───────────┐
  │    LLM    │                   │    gcc    │
  └─────┬─────┘                   └─────┬─────┘
        │                               │
        ▼                               ▼
     C 代码                           二进制
```

当然，LLM 和 gcc 有一个很大的不同：gcc 的行为是确定性的，同样的输入永远得到同样的输出。LLM 不是。它会犯错，会误解你的意思，会在你没注意的地方做出错误的假设。所以你需要读它生成的代码，需要理解它在做什么，需要在它犯错的时候能指出问题。这也是为什么前面每一节都在帮你建立对系统的理解：不是为了让你手写每一行代码，而是为了让你有能力判断 AI 写的代码对不对。

如果你真的把前面每个问题都想清楚了，你会发现一件事：打开 AI 生成的代码，你居然能读懂。不是每一行都懂，但你知道每个函数在干什么、为什么在那里。这就是”指挥”和”复制粘贴”的区别。

### 那手写代码不是更扎实吗？

也许你会觉得：老老实实手写代码，一行一行记住，不是更扎实吗？确实，手写能帮你记住 `setsid()` 怎么拼、`poll()` 的参数是什么。但这些东西查一下文档就知道了。而理解 Client 和 Server 为什么要分开、PTY 为什么比 pipe 好、fd 泄漏为什么会导致僵尸，这些东西 AI 经常答错，查文档也查不到现成答案，得你自己想明白。想明白了，你就能在 AI 犯错的时候看出来，能在系统出了诡异 bug 的时候知道往哪查。

这个 Lab 试图把时间花在后者上。AI 能做好的事交给 AI，省下来的时间花在理解系统上。

### 我们听到了

我们也知道实际体验离这个理想还有距离。前置知识多、AI 生成的代码太长读不懂、最后变成无脑点 yes，这些反馈我们都收到了，问卷里的每一条我们都认真看了。如果你还没填，希望你也去写几句，匿名的，真的会看。说实话这是第一次用这种方式设计 Lab，助教团队内部也吵过很多次，到底该不该这么做、会不会适得其反。评分已经做了调整（降低了基础门槛，公开了部分评测用例和评测方式），这篇文档也在试着帮你建立”先理解再动手”的节奏。后面还会继续调整，这个过程不是我们单方面拍板，你的反馈真的在影响我们的决策。

### 为什么我们坚持拥抱 AI

但有一件事是整个助教团队讨论后一致的判断：我们不能假装 AI 不存在。ICS 的全称是 Introduction to Computer Systems，这是一门系统课，也是一门实践课。ICS2 这学期跟的是 OSTEP（Operating Systems: Three Easy Pieces），这本书本身就不是让你背概念的，而是带你一层一层理解操作系统怎么运转。这个 Lab 做的事情和 OSTEP 的精神一样：动手造东西，踩坑，debug，在过程中理解系统。AI 改变的不是这个学科的本质，而是你造东西的方式。AI 不会消失，只会越来越强。怎么在 AI 的帮助下理解和构建一个真实系统，这本身就是这门课要教的事情。我们选择在课堂上和你一起趟这条路，一起踩坑，一起摸索出一套行得通的方法。这不是一个已经想好了的完美方案，而是我们和你共同参与的一次实验。

### 你做的事情是真的

你可能觉得这只是一个课程作业。但回头看看你刚做的事情：理解一个 C/S 系统的架构，设计通信协议，调试 fd 泄漏，处理信号隔离。这些就是真实世界里系统工程师每天在做的事情，区别只是规模，思维方式完全一样。这个 Lab 不是在模拟什么，它就是系统编程本身。

### ☕️

我们知道你们这一届赶上了一个很特殊的时候。AI 的能力在过去两年里爆发式增长，整个行业都在剧烈变化，课程在变，工具在变，连”什么才算会编程”这个问题的答案都在变。这种不确定性带来的焦虑和不安，我们感同身受。作为助教，我们自己也在被这股浪潮冲着走，也在想自己以后该往哪去。所以请相信，我们不是站在岸上给你指路的人，我们就在水里，和你一起。
