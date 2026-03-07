#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#endif
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr const char* kDefaultServerName = "default";
constexpr size_t kMaxBufferPerPane = 1 << 20;  // 1 MiB

enum MsgType : uint32_t {
  MSG_ATTACH = 1,
  MSG_INPUT = 2,
  MSG_CMD = 3,
  MSG_RESIZE = 4,
  MSG_OUTPUT = 5,
  MSG_ERROR = 6,
};

struct MsgHeader {
  uint32_t type;
  uint32_t len;
};

struct ResizePayload {
  uint32_t rows;
  uint32_t cols;
};

struct AttachPayload {
  uint32_t flags;  // bit0: readonly
};

struct Pane {
  int id = -1;
  int pty_master_fd = -1;
  pid_t child_pid = -1;
  pid_t pgid = -1;
  std::string buffer;

  int log_fd = -1;

  pid_t pipeout_pid = -1;
  int pipeout_fd = -1;
};

struct ClientConn {
  int fd = -1;
  bool readonly = false;
  int rows = 24;
  int cols = 80;
};

struct ServerState {
  std::unordered_map<int, Pane> panes;
  int focused_pane_id = -1;
  int next_pane_id = 0;
  int listen_fd = -1;
  std::vector<ClientConn> clients;
  int rows = 24;
  int cols = 80;
  int sigchld_pipe[2] = {-1, -1};
};

volatile sig_atomic_t g_client_resize_flag = 0;

void OnClientWinch(int) { g_client_resize_flag = 1; }

ssize_t SendAll(int fd, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, p + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    sent += static_cast<size_t>(n);
  }
  return static_cast<ssize_t>(sent);
}

bool RecvFull(int fd, void* data, size_t len) {
  char* p = static_cast<char*>(data);
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, p + got, len - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

bool SendMsg(int fd, MsgType type, const void* payload, uint32_t len) {
  MsgHeader h;
  h.type = htonl(static_cast<uint32_t>(type));
  h.len = htonl(len);
  if (SendAll(fd, &h, sizeof(h)) < 0) return false;
  if (len > 0 && payload != nullptr) {
    if (SendAll(fd, payload, len) < 0) return false;
  }
  return true;
}

bool RecvMsg(int fd, MsgType* type, std::string* payload) {
  MsgHeader h;
  if (!RecvFull(fd, &h, sizeof(h))) return false;
  uint32_t t = ntohl(h.type);
  uint32_t len = ntohl(h.len);
  payload->clear();
  if (len > 0) {
    payload->resize(len);
    if (!RecvFull(fd, payload->data(), len)) return false;
  }
  *type = static_cast<MsgType>(t);
  return true;
}

int SetNonBlock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string GetServerName() {
  const char* env = getenv("MINI_TMUX_SERVER");
  if (env == nullptr || *env == '\0') return kDefaultServerName;
  return std::string(env);
}

std::string BuildSocketPath() {
  std::string name = GetServerName();
  for (char& c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
  }
  return "/tmp/mini_tmux_" + std::to_string(getuid()) + "_" + name + ".sock";
}

void AppendPaneBuffer(Pane* p, const char* data, size_t n) {
  if (n == 0) return;
  if (n >= kMaxBufferPerPane) {
    p->buffer.assign(data + (n - kMaxBufferPerPane), data + n);
    return;
  }
  if (p->buffer.size() + n > kMaxBufferPerPane) {
    size_t drop = p->buffer.size() + n - kMaxBufferPerPane;
    p->buffer.erase(0, drop);
  }
  p->buffer.append(data, n);
}

void StopPaneLog(Pane* p) {
  if (p->log_fd >= 0) {
    close(p->log_fd);
    p->log_fd = -1;
  }
}

void StopPanePipeout(Pane* p) {
  if (p->pipeout_fd >= 0) {
    close(p->pipeout_fd);
    p->pipeout_fd = -1;
  }
  if (p->pipeout_pid > 0) {
    int st = 0;
    const int kPollRounds = 50;  // ~500ms total
    bool exited = false;
    for (int i = 0; i < kPollRounds; ++i) {
      pid_t r = waitpid(p->pipeout_pid, &st, WNOHANG);
      if (r == p->pipeout_pid) {
        exited = true;
        break;
      }
      if (r < 0 && errno != EINTR) {
        exited = true;
        break;
      }
      usleep(10000);
    }
    if (!exited) {
      kill(p->pipeout_pid, SIGTERM);
      for (int i = 0; i < kPollRounds; ++i) {
        pid_t r = waitpid(p->pipeout_pid, &st, WNOHANG);
        if (r == p->pipeout_pid) {
          exited = true;
          break;
        }
        if (r < 0 && errno != EINTR) {
          exited = true;
          break;
        }
        usleep(10000);
      }
    }
    if (!exited) {
      kill(p->pipeout_pid, SIGKILL);
      while (waitpid(p->pipeout_pid, &st, 0) < 0 && errno == EINTR) {
      }
    }
    p->pipeout_pid = -1;
  }
}

void StopPanePipeoutIfDead(Pane* p, pid_t pid) {
  if (p->pipeout_pid == pid) {
    if (p->pipeout_fd >= 0) {
      close(p->pipeout_fd);
      p->pipeout_fd = -1;
    }
    p->pipeout_pid = -1;
  }
}

void RemoveClientByIndex(ServerState* s, size_t idx) {
  if (idx >= s->clients.size()) return;
  if (s->clients[idx].fd >= 0) close(s->clients[idx].fd);
  s->clients.erase(s->clients.begin() + static_cast<long>(idx));
}

bool SendMsgToClient(ClientConn* c, MsgType type, const void* payload, uint32_t len) {
  if (c->fd < 0) return false;
  return SendMsg(c->fd, type, payload, len);
}

void BroadcastToAllClients(ServerState* s, MsgType type, const void* payload, uint32_t len) {
  for (size_t i = 0; i < s->clients.size();) {
    if (!SendMsgToClient(&s->clients[i], type, payload, len)) {
      RemoveClientByIndex(s, i);
      continue;
    }
    ++i;
  }
}

bool SendFocusedBufferToClient(ServerState* s, ClientConn* c) {
  if (s->focused_pane_id < 0) return true;
  auto it = s->panes.find(s->focused_pane_id);
  if (it == s->panes.end()) return true;

  std::string payload;
  payload.reserve(32 + it->second.buffer.size());
  payload += "\x1b[2J\x1b[H";
  payload += "[mini-tmux] focus pane " + std::to_string(s->focused_pane_id) + "\r\n";
  payload += it->second.buffer;
  return SendMsgToClient(c, MSG_OUTPUT, payload.data(), static_cast<uint32_t>(payload.size()));
}

void SendFocusedBufferToAll(ServerState* s) {
  for (size_t i = 0; i < s->clients.size();) {
    if (!SendFocusedBufferToClient(s, &s->clients[i])) {
      RemoveClientByIndex(s, i);
      continue;
    }
    ++i;
  }
}

void BroadcastPaneData(ServerState* s, int pane_id, const char* data, size_t n) {
  auto it = s->panes.find(pane_id);
  if (it == s->panes.end()) return;
  Pane* p = &it->second;

  AppendPaneBuffer(p, data, n);

  if (p->log_fd >= 0) {
    ssize_t wr = write(p->log_fd, data, n);
    (void)wr;
  }

  if (p->pipeout_fd >= 0) {
    ssize_t wr = write(p->pipeout_fd, data, n);
    if (wr < 0 && (errno == EPIPE || errno == EBADF)) {
      if (p->pipeout_fd >= 0) {
        close(p->pipeout_fd);
        p->pipeout_fd = -1;
      }
    }
  }

  BroadcastToAllClients(s, MSG_OUTPUT, data, static_cast<uint32_t>(n));
}

void UpdatePaneWinsizeAndNotify(Pane* p, int rows, int cols) {
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = static_cast<unsigned short>(rows);
  ws.ws_col = static_cast<unsigned short>(cols);
  ioctl(p->pty_master_fd, TIOCSWINSZ, &ws);
  if (p->pgid > 0) kill(-p->pgid, SIGWINCH);
}

int CreatePane(ServerState* s) {
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = static_cast<unsigned short>(s->rows);
  ws.ws_col = static_cast<unsigned short>(s->cols);

  int master = -1;
  int slave = -1;
  if (openpty(&master, &slave, nullptr, nullptr, &ws) < 0) {
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(master);
    close(slave);
    return -1;
  }

  if (pid == 0) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGWINCH, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    setsid();
    setpgid(0, 0);
    ioctl(slave, TIOCSCTTY, 0);
    tcsetpgrp(slave, getpid());

    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO) close(slave);
    close(master);

    const char* shell = getenv("SHELL");
    if (shell == nullptr || *shell == '\0') shell = "/bin/sh";
    execlp(shell, shell, static_cast<char*>(nullptr));
    execlp("/bin/sh", "sh", static_cast<char*>(nullptr));
    _exit(127);
  }

  close(slave);
  SetNonBlock(master);

  Pane pane;
  pane.id = s->next_pane_id++;
  pane.pty_master_fd = master;
  pane.child_pid = pid;
  pane.pgid = pid;

  int id = pane.id;
  s->panes[id] = std::move(pane);
  if (s->focused_pane_id < 0) s->focused_pane_id = id;

  return id;
}

void DestroyPane(ServerState* s, int pane_id) {
  auto it = s->panes.find(pane_id);
  if (it == s->panes.end()) return;

  Pane p = std::move(it->second);
  s->panes.erase(it);

  StopPaneLog(&p);

  if (p.pipeout_fd >= 0) {
    close(p.pipeout_fd);
    p.pipeout_fd = -1;
  }

  if (p.pgid > 0) {
    kill(-p.pgid, SIGKILL);
  } else if (p.child_pid > 0) {
    kill(p.child_pid, SIGKILL);
  }

  if (p.pty_master_fd >= 0) close(p.pty_master_fd);

  if (p.pipeout_pid > 0) {
    int st = 0;
    while (waitpid(p.pipeout_pid, &st, 0) < 0 && errno == EINTR) {
    }
  }

  if (s->focused_pane_id == pane_id) {
    if (s->panes.empty()) {
      s->focused_pane_id = -1;
    } else {
      int best = s->panes.begin()->first;
      for (const auto& kv : s->panes) best = std::min(best, kv.first);
      s->focused_pane_id = best;
    }
    SendFocusedBufferToAll(s);
  }
}

void FocusPane(ServerState* s, int pane_id) {
  if (s->panes.find(pane_id) == s->panes.end()) return;
  s->focused_pane_id = pane_id;
  SendFocusedBufferToAll(s);
}

void FocusNext(ServerState* s, bool forward) {
  if (s->panes.empty()) return;
  std::vector<int> ids;
  ids.reserve(s->panes.size());
  for (const auto& kv : s->panes) ids.push_back(kv.first);
  std::sort(ids.begin(), ids.end());
  auto it = std::find(ids.begin(), ids.end(), s->focused_pane_id);
  if (it == ids.end()) {
    s->focused_pane_id = ids.front();
    SendFocusedBufferToAll(s);
    return;
  }
  size_t pos = static_cast<size_t>(it - ids.begin());
  if (forward) {
    pos = (pos + 1) % ids.size();
  } else {
    pos = (pos + ids.size() - 1) % ids.size();
  }
  s->focused_pane_id = ids[pos];
  SendFocusedBufferToAll(s);
}

int g_sigchld_write_fd = -1;

void ServerSigchldHandlerSimple(int) {
  if (g_sigchld_write_fd >= 0) {
    char b = 'x';
    ssize_t wr = write(g_sigchld_write_fd, &b, 1);
    (void)wr;
  }
}

void ReapChildren(ServerState* s) {
  while (true) {
    int st = 0;
    pid_t pid = waitpid(-1, &st, WNOHANG);
    if (pid <= 0) break;

    int pane_to_remove = -1;
    for (auto& kv : s->panes) {
      Pane& p = kv.second;
      if (p.child_pid == pid) {
        pane_to_remove = p.id;
        break;
      }
      if (p.pipeout_pid == pid) {
        StopPanePipeoutIfDead(&p, pid);
        break;
      }
    }
    if (pane_to_remove >= 0) {
      DestroyPane(s, pane_to_remove);
    }
  }
}

void ApplyResizeToAll(ServerState* s, int rows, int cols) {
  s->rows = rows;
  s->cols = cols;
  for (auto& kv : s->panes) {
    UpdatePaneWinsizeAndNotify(&kv.second, rows, cols);
  }
}

void RecomputeGlobalSize(ServerState* s) {
  if (s->clients.empty()) {
    ApplyResizeToAll(s, 24, 80);
    return;
  }
  int min_rows = s->clients[0].rows;
  int min_cols = s->clients[0].cols;
  for (const auto& c : s->clients) {
    min_rows = std::min(min_rows, c.rows);
    min_cols = std::min(min_cols, c.cols);
  }
  if (min_rows <= 0) min_rows = 24;
  if (min_cols <= 0) min_cols = 80;
  ApplyResizeToAll(s, min_rows, min_cols);
}

void HandleClientInputToPane(ServerState* s, const std::string& bytes) {
  if (s->focused_pane_id < 0) return;
  auto it = s->panes.find(s->focused_pane_id);
  if (it == s->panes.end()) return;
  Pane* p = &it->second;

  auto current_foreground_pgid = [&]() -> pid_t {
    pid_t fg = tcgetpgrp(p->pty_master_fd);
    if (fg > 0) return fg;
    return p->pgid;
  };

  std::string pending;
  pending.reserve(bytes.size());

  auto flush_pending = [&]() {
    if (pending.empty()) return;
    size_t off = 0;
    while (off < pending.size()) {
      ssize_t wr = write(p->pty_master_fd, pending.data() + off, pending.size() - off);
      if (wr > 0) {
        off += static_cast<size_t>(wr);
        continue;
      }
      if (wr < 0 && errno == EINTR) continue;
      if (wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        usleep(1000);
        continue;
      }
      break;
    }
    pending.clear();
  };

  for (unsigned char ch : bytes) {
    if (ch == 0x03) {
      flush_pending();
      pid_t fg = current_foreground_pgid();
      if (fg > 0) kill(-fg, SIGINT);
      continue;
    }
    if (ch == 0x1A) {
      flush_pending();
      pid_t fg = current_foreground_pgid();
      if (fg > 0) kill(-fg, SIGTSTP);
      continue;
    }
    if (ch == '\r') {
      pending.push_back('\n');
    } else {
      pending.push_back(static_cast<char>(ch));
    }
  }
  flush_pending();
}

std::string StripAnsi(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c != 0x1b) {
      out.push_back(in[i++]);
      continue;
    }
    if (i + 1 >= in.size()) break;
    unsigned char n = static_cast<unsigned char>(in[i + 1]);
    if (n == '[') {
      i += 2;
      while (i < in.size()) {
        unsigned char t = static_cast<unsigned char>(in[i]);
        if (t >= 0x40 && t <= 0x7E) {
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (n == ']') {
      i += 2;
      while (i < in.size()) {
        unsigned char t = static_cast<unsigned char>(in[i]);
        if (t == 0x07) {
          ++i;
          break;
        }
        if (t == 0x1b && i + 1 < in.size() && in[i + 1] == '\\') {
          i += 2;
          break;
        }
        ++i;
      }
      continue;
    }
    i += 2;
  }
  return out;
}

void CapturePaneToFile(ServerState* s, int pane_id, const std::string& path) {
  auto it = s->panes.find(pane_id);
  if (it == s->panes.end()) return;
  std::string text = StripAnsi(it->second.buffer);
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;
  SendAll(fd, text.data(), text.size());
  close(fd);
}

void HandleCommand(ServerState* s, const std::string& cmd_raw) {
  std::string cmd = cmd_raw;
  while (!cmd.empty() && std::isspace(static_cast<unsigned char>(cmd.back()))) cmd.pop_back();
  size_t i = 0;
  while (i < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[i]))) ++i;
  cmd = cmd.substr(i);
  if (cmd.empty()) return;

  auto starts_with = [&](const std::string& pre) { return cmd.rfind(pre, 0) == 0; };

  if (cmd == "new") {
    int id = CreatePane(s);
    if (id >= 0) FocusPane(s, id);
    return;
  }
  if (starts_with("kill ")) {
    int id = atoi(cmd.c_str() + 5);
    DestroyPane(s, id);
    return;
  }
  if (starts_with("focus ")) {
    int id = atoi(cmd.c_str() + 6);
    FocusPane(s, id);
    return;
  }
  if (cmd == "next") {
    FocusNext(s, true);
    return;
  }
  if (cmd == "prev") {
    FocusNext(s, false);
    return;
  }
  if (starts_with("log-stop ")) {
    int id = atoi(cmd.c_str() + 9);
    auto it = s->panes.find(id);
    if (it != s->panes.end()) StopPaneLog(&it->second);
    return;
  }
  if (starts_with("log ")) {
    size_t sp = cmd.find(' ', 4);
    if (sp == std::string::npos) return;
    int id = atoi(cmd.c_str() + 4);
    std::string path = cmd.substr(sp + 1);
    auto it = s->panes.find(id);
    if (it == s->panes.end()) return;
    Pane* p = &it->second;
    StopPaneLog(p);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) p->log_fd = fd;
    return;
  }
  if (starts_with("pipeout-stop ")) {
    int id = atoi(cmd.c_str() + 13);
    auto it = s->panes.find(id);
    if (it != s->panes.end()) StopPanePipeout(&it->second);
    return;
  }
  if (starts_with("pipeout ")) {
    size_t sp = cmd.find(' ', 8);
    if (sp == std::string::npos) return;
    int id = atoi(cmd.c_str() + 8);
    std::string sh_cmd = cmd.substr(sp + 1);
    auto it = s->panes.find(id);
    if (it == s->panes.end()) return;
    Pane* p = &it->second;

    StopPanePipeout(p);

    int fds[2] = {-1, -1};
    if (pipe(fds) < 0) return;

    pid_t cpid = fork();
    if (cpid < 0) {
      close(fds[0]);
      close(fds[1]);
      return;
    }

    if (cpid == 0) {
      dup2(fds[0], STDIN_FILENO);
      close(fds[0]);
      close(fds[1]);
      execl("/bin/sh", "sh", "-c", sh_cmd.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }

    close(fds[0]);
    p->pipeout_pid = cpid;
    p->pipeout_fd = fds[1];
    SetNonBlock(p->pipeout_fd);
    return;
  }
  if (starts_with("capture ")) {
    size_t sp = cmd.find(' ', 8);
    if (sp == std::string::npos) return;
    int id = atoi(cmd.c_str() + 8);
    std::string path = cmd.substr(sp + 1);
    CapturePaneToFile(s, id, path);
    return;
  }
}

int CreateListenSocket(const std::string& sock_path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

  unlink(sock_path.c_str());
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  chmod(sock_path.c_str(), 0600);

  if (listen(fd, 16) < 0) {
    close(fd);
    unlink(sock_path.c_str());
    return -1;
  }
  return fd;
}

int ConnectSocket(const std::string& sock_path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int RunServer(const std::string& sock_path) {
  signal(SIGPIPE, SIG_IGN);

  ServerState s;
  s.listen_fd = CreateListenSocket(sock_path);
  if (s.listen_fd < 0) {
    std::cerr << "failed to create server socket\n";
    return 1;
  }

  if (pipe(s.sigchld_pipe) < 0) {
    std::cerr << "failed to create sigchld pipe\n";
    close(s.listen_fd);
    unlink(sock_path.c_str());
    return 1;
  }
  SetNonBlock(s.sigchld_pipe[0]);
  SetNonBlock(s.sigchld_pipe[1]);
  g_sigchld_write_fd = s.sigchld_pipe[1];

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = ServerSigchldHandlerSimple;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, nullptr);

  int p0 = CreatePane(&s);
  if (p0 >= 0) FocusPane(&s, p0);

  while (true) {
    std::vector<struct pollfd> pfds;
    pfds.reserve(2 + s.clients.size() + s.panes.size());

    pfds.push_back({s.listen_fd, POLLIN, 0});
    pfds.push_back({s.sigchld_pipe[0], POLLIN, 0});

    std::vector<size_t> client_idx_by_poll;
    client_idx_by_poll.reserve(s.clients.size());
    for (size_t i = 0; i < s.clients.size(); ++i) {
      client_idx_by_poll.push_back(i);
      pfds.push_back({s.clients[i].fd, POLLIN | POLLHUP | POLLERR, 0});
    }
    size_t client_poll_count = client_idx_by_poll.size();

    std::vector<int> pane_ids;
    pane_ids.reserve(s.panes.size());
    for (const auto& kv : s.panes) {
      pane_ids.push_back(kv.first);
      pfds.push_back({kv.second.pty_master_fd, POLLIN | POLLHUP | POLLERR, 0});
    }

    int rc = poll(pfds.data(), pfds.size(), 500);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (pfds[0].revents & POLLIN) {
      int cfd = accept(s.listen_fd, nullptr, nullptr);
      if (cfd >= 0) {
        ClientConn cc;
        cc.fd = cfd;
        cc.readonly = false;
        s.clients.push_back(cc);
      }
    }

    if (pfds[1].revents & POLLIN) {
      char tmp[64];
      while (read(s.sigchld_pipe[0], tmp, sizeof(tmp)) > 0) {
      }
      ReapChildren(&s);
    }

    size_t client_base = 2;
    for (size_t pi = 0; pi < client_idx_by_poll.size();) {
      size_t poll_index = client_base + pi;
      size_t cidx = client_idx_by_poll[pi];
      if (cidx >= s.clients.size()) {
        ++pi;
        continue;
      }
      short re = pfds[poll_index].revents;
      bool removed = false;
      if (re & (POLLHUP | POLLERR)) {
        RemoveClientByIndex(&s, cidx);
        RecomputeGlobalSize(&s);
        removed = true;
      } else if (re & POLLIN) {
        MsgType t;
        std::string payload;
        if (!RecvMsg(s.clients[cidx].fd, &t, &payload)) {
          RemoveClientByIndex(&s, cidx);
          RecomputeGlobalSize(&s);
          removed = true;
        } else {
          if (t == MSG_ATTACH) {
            if (payload.size() == sizeof(AttachPayload)) {
              AttachPayload ap;
              memcpy(&ap, payload.data(), sizeof(ap));
              uint32_t flags = ntohl(ap.flags);
              s.clients[cidx].readonly = (flags & 1U) != 0;
            }
            SendFocusedBufferToClient(&s, &s.clients[cidx]);
          } else if (t == MSG_INPUT) {
            if (!s.clients[cidx].readonly) HandleClientInputToPane(&s, payload);
          } else if (t == MSG_CMD) {
            if (!s.clients[cidx].readonly) HandleCommand(&s, payload);
          } else if (t == MSG_RESIZE) {
            if (payload.size() == sizeof(ResizePayload)) {
              ResizePayload rp;
              memcpy(&rp, payload.data(), sizeof(rp));
              int rows = static_cast<int>(ntohl(rp.rows));
              int cols = static_cast<int>(ntohl(rp.cols));
              if (rows > 0 && cols > 0) {
                s.clients[cidx].rows = rows;
                s.clients[cidx].cols = cols;
                RecomputeGlobalSize(&s);
              }
            }
          }
        }
      }
      if (removed) {
        client_idx_by_poll.erase(client_idx_by_poll.begin() + static_cast<long>(pi));
        for (size_t& mapped : client_idx_by_poll) {
          if (mapped > cidx) --mapped;
        }
        continue;
      }
      ++pi;
    }

    size_t pane_base = 2 + client_poll_count;
    for (size_t i = 0; i < pane_ids.size(); ++i) {
      short re = pfds[pane_base + i].revents;
      int pane_id = pane_ids[i];
      auto it = s.panes.find(pane_id);
      if (it == s.panes.end()) continue;
      Pane* p = &it->second;

      if (re & POLLIN) {
        char buf[8192];
        while (true) {
          ssize_t n = read(p->pty_master_fd, buf, sizeof(buf));
          if (n > 0) {
            BroadcastPaneData(&s, pane_id, buf, static_cast<size_t>(n));
            continue;
          }
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
          break;
        }
      }

      if (re & (POLLHUP | POLLERR)) {
        // Will be cleaned by SIGCHLD reap; keep loop stable here.
      }
    }
  }

  while (!s.clients.empty()) RemoveClientByIndex(&s, 0);
  close(s.listen_fd);
  close(s.sigchld_pipe[0]);
  close(s.sigchld_pipe[1]);
  unlink(sock_path.c_str());
  return 0;
}

class RawTerminalGuard {
 public:
  RawTerminalGuard() = default;
  bool Enable() {
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &old_) < 0) return false;
    termios raw = old_;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return false;
    enabled_ = true;
    return true;
  }
  ~RawTerminalGuard() {
    if (enabled_) tcsetattr(STDIN_FILENO, TCSANOW, &old_);
  }

 private:
  termios old_{};
  bool enabled_ = false;
};

void SendResizeMsg(int sockfd) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) return;
  if (ws.ws_row == 0 || ws.ws_col == 0) return;
  ResizePayload rp;
  rp.rows = htonl(ws.ws_row);
  rp.cols = htonl(ws.ws_col);
  SendMsg(sockfd, MSG_RESIZE, &rp, sizeof(rp));
}

int RunClient(const std::string& sock_path, bool readonly) {
  signal(SIGPIPE, SIG_IGN);

  int sockfd = -1;
  for (int i = 0; i < 200; ++i) {
    sockfd = ConnectSocket(sock_path);
    if (sockfd >= 0) break;
    usleep(10000);
  }
  if (sockfd < 0) {
    std::cerr << "cannot connect to server\n";
    return 1;
  }

  RawTerminalGuard tty;
  tty.Enable();

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = OnClientWinch;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGWINCH, &sa, nullptr);

  AttachPayload ap;
  ap.flags = htonl(readonly ? 1U : 0U);
  SendMsg(sockfd, MSG_ATTACH, &ap, sizeof(ap));
  SendResizeMsg(sockfd);

  bool prefix_mode = false;
  int prefix_esc_state = 0;  // 0:none, 1:got ESC after prefix, 2:got ESC[
  bool cmd_mode = false;
  std::string cmd_buf;
  bool at_line_start = true;
  auto is_control_command = [](const std::string& cmd) {
    if (cmd == "new" || cmd == "next" || cmd == "prev") return true;
    if (cmd.rfind("kill ", 0) == 0) return true;
    if (cmd.rfind("focus ", 0) == 0) return true;
    if (cmd.rfind("log ", 0) == 0) return true;
    if (cmd.rfind("log-stop ", 0) == 0) return true;
    if (cmd.rfind("pipeout ", 0) == 0) return true;
    if (cmd.rfind("pipeout-stop ", 0) == 0) return true;
    if (cmd.rfind("capture ", 0) == 0) return true;
    return false;
  };

  while (true) {
    if (g_client_resize_flag) {
      g_client_resize_flag = 0;
      SendResizeMsg(sockfd);
    }

    struct pollfd pfds[2];
    pfds[0].fd = STDIN_FILENO;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = sockfd;
    pfds[1].events = POLLIN | POLLHUP | POLLERR;
    pfds[1].revents = 0;

    int rc = poll(pfds, 2, 200);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (pfds[1].revents & (POLLHUP | POLLERR)) {
      break;
    }

    if (pfds[1].revents & POLLIN) {
      MsgType t;
      std::string payload;
      if (!RecvMsg(sockfd, &t, &payload)) break;
      if (t == MSG_OUTPUT) {
        SendAll(STDOUT_FILENO, payload.data(), payload.size());
      } else if (t == MSG_ERROR) {
        std::string line = "\r\n[mini-tmux] " + payload + "\r\n";
        SendAll(STDOUT_FILENO, line.data(), line.size());
      }
    }

    if (pfds[0].revents & POLLIN) {
      char buf[1024];
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) {
        continue;
      }

      for (ssize_t i = 0; i < n; ++i) {
        unsigned char ch = static_cast<unsigned char>(buf[i]);

        if (cmd_mode) {
          if (ch == '\r' || ch == '\n') {
            if (is_control_command(cmd_buf)) {
              if (!readonly) {
                SendMsg(sockfd, MSG_CMD, cmd_buf.data(), static_cast<uint32_t>(cmd_buf.size()));
              }
            } else {
              if (!readonly) {
                std::string forward = ":" + cmd_buf + "\n";
                SendMsg(sockfd, MSG_INPUT, forward.data(), static_cast<uint32_t>(forward.size()));
              }
            }
            cmd_mode = false;
            cmd_buf.clear();
            at_line_start = true;
            const char* crlf = "\r\n";
            SendAll(STDOUT_FILENO, crlf, 2);
            continue;
          }
          if (ch == 0x7f || ch == 0x08) {
            if (!cmd_buf.empty()) {
              cmd_buf.pop_back();
              const char* bs = "\b \b";
              SendAll(STDOUT_FILENO, bs, 3);
            }
            continue;
          }
          if (ch >= 32 && ch != 127) {
            cmd_buf.push_back(static_cast<char>(ch));
            SendAll(STDOUT_FILENO, &ch, 1);
          }
          continue;
        }

        if (prefix_mode) {
          if (prefix_esc_state == 0) {
            if (ch == 'd' || ch == 'D') {
              close(sockfd);
              return 0;
            }
            if (ch == 'n' || ch == 'N') {
              const char* c = "next";
              if (!readonly) SendMsg(sockfd, MSG_CMD, c, 4);
              prefix_mode = false;
              continue;
            }
            if (ch == 'p' || ch == 'P') {
              const char* c = "prev";
              if (!readonly) SendMsg(sockfd, MSG_CMD, c, 4);
              prefix_mode = false;
              continue;
            }
            if (ch == 27) {
              prefix_esc_state = 1;
              continue;
            }
            prefix_mode = false;
            continue;
          }
          if (prefix_esc_state == 1) {
            if (ch == '[') {
              prefix_esc_state = 2;
              continue;
            }
            prefix_mode = false;
            prefix_esc_state = 0;
            continue;
          }
          if (prefix_esc_state == 2) {
            if (ch == 'C' || ch == 'B') {
              const char* c = "next";
              if (!readonly) SendMsg(sockfd, MSG_CMD, c, 4);
            } else if (ch == 'D' || ch == 'A') {
              const char* c = "prev";
              if (!readonly) SendMsg(sockfd, MSG_CMD, c, 4);
            }
            prefix_mode = false;
            prefix_esc_state = 0;
            continue;
          }
        }

        if (ch == 0x02) {
          prefix_mode = true;
          prefix_esc_state = 0;
          continue;
        }

        if (ch == ':' && at_line_start) {
          cmd_mode = true;
          cmd_buf.clear();
          SendAll(STDOUT_FILENO, &ch, 1);
          continue;
        }

        if (!readonly) SendMsg(sockfd, MSG_INPUT, &ch, 1);
        at_line_start = (ch == '\r' || ch == '\n');
      }
    }
  }

  close(sockfd);
  return 0;
}

int StartServerAndAttach(const std::string& self_path, const std::string& sock_path) {
  int existing = ConnectSocket(sock_path);
  if (existing >= 0) {
    close(existing);
    return RunClient(sock_path, false);
  }

  pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "failed to fork server\n";
    return 1;
  }

  if (pid == 0) {
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) close(devnull);
    }
    execl(self_path.c_str(), self_path.c_str(), "--server", sock_path.c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }

  return RunClient(sock_path, false);
}

}  // namespace

int main(int argc, char** argv) {
  std::string sock_path = BuildSocketPath();

  if (argc == 3 && std::string(argv[1]) == "--server") {
    return RunServer(argv[2]);
  }

  if (argc == 1) {
    return StartServerAndAttach(argv[0], sock_path);
  }

  if (argc == 2 && std::string(argv[1]) == "attach") {
    return RunClient(sock_path, false);
  }

  if (argc == 3 && std::string(argv[1]) == "attach" && std::string(argv[2]) == "-r") {
    return RunClient(sock_path, true);
  }

  std::cerr << "usage: " << argv[0] << " [attach [-r]]\n";
  return 1;
}
