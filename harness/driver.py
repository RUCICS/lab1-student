"""Driver: controllers for tmux and mini-tmux client-server lifecycle."""
from __future__ import annotations

import os
import subprocess
import time
from typing import Optional

import pexpect
import psutil


class TmuxDriver:
    """Drives real tmux purely via CLI (no pexpect fork, avoids forkpty issues)."""

    def __init__(
        self,
        server_name: Optional[str] = None,
        rows: int = 24,
        cols: int = 80,
    ) -> None:
        self._server_name = server_name or f"harness_{os.getpid()}"
        self._rows = rows
        self._cols = cols
        self._server_pid_cache: Optional[int] = None
        self._extra_clients: list[pexpect.spawn] = []

    def _tmux(self, *args: str, check: bool = True) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["tmux", "-L", self._server_name, *args],
            capture_output=True,
            text=True,
            check=check,
        )

    def start(self) -> None:
        self._tmux(
            "new-session", "-d",
            "-x", str(self._cols), "-y", str(self._rows),
        )
        for _ in range(20):
            r = self._tmux("list-sessions", check=False)
            if r.returncode == 0:
                break
            time.sleep(0.1)
        time.sleep(0.3)

    def server_pid(self) -> int:
        result = self._tmux("display-message", "-p", "#{pid}", check=False)
        if result.returncode == 0 and result.stdout.strip():
            pid = int(result.stdout.strip())
            self._server_pid_cache = pid
            return pid
        raise RuntimeError("Cannot find tmux server PID")

    def server_alive(self) -> bool:
        if self._server_pid_cache:
            return psutil.pid_exists(self._server_pid_cache)
        try:
            self.server_pid()
            return True
        except RuntimeError:
            return False

    def client_alive(self) -> bool:
        r = self._tmux("list-sessions", check=False)
        return r.returncode == 0

    def detach(self) -> None:
        pass

    def reattach(self) -> None:
        pass

    def send_keys(self, keys: str) -> None:
        self._tmux("send-keys", keys, check=False)

    def send_line(self, text: str) -> None:
        self._tmux("send-keys", text, "Enter", check=False)

    def read_raw(self, timeout: float = 1.0) -> bytes:
        time.sleep(timeout)
        result = self._tmux("capture-pane", "-p", "-e", check=False)
        return result.stdout.encode() if result.returncode == 0 else b""

    def resize(self, rows: int, cols: int) -> None:
        self._rows = rows
        self._cols = cols
        self._tmux(
            "resize-window", "-x", str(cols), "-y", str(rows),
            check=False,
        )

    def attach_client(self, readonly: bool = False, rows: int = 0, cols: int = 0) -> int:
        r = rows or self._rows
        c = cols or self._cols
        args = f"tmux -L {self._server_name} attach"
        if readonly:
            args += " -r"
        child = pexpect.spawn(args, dimensions=(r, c), encoding=None, timeout=5)
        child.delaybeforesend = 0.05
        time.sleep(0.3)
        self._extra_clients.append(child)
        return len(self._extra_clients) - 1

    def new_session_client(self, rows: int = 0, cols: int = 0) -> int:
        r = rows or self._rows
        c = cols or self._cols
        session_name = f"s{len(self._extra_clients) + 1}"
        cmd = f"tmux -L {self._server_name} new-session -s {session_name}"
        child = pexpect.spawn(cmd, dimensions=(r, c), encoding=None, timeout=5)
        child.delaybeforesend = 0.05
        time.sleep(0.5)
        self._extra_clients.append(child)
        return len(self._extra_clients) - 1

    def detach_extra_client(self, idx: int) -> None:
        if idx < len(self._extra_clients) and self._extra_clients[idx]:
            child = self._extra_clients[idx]
            child.send(b"\x02")
            time.sleep(0.1)
            child.send(b"d")
            try:
                child.expect(pexpect.EOF, timeout=3)
            except pexpect.TIMEOUT:
                pass
            time.sleep(0.2)

    def kill_extra_client(self, idx: int) -> None:
        if idx < len(self._extra_clients) and self._extra_clients[idx]:
            self._extra_clients[idx].terminate(force=True)
            time.sleep(0.2)

    def read_extra_client(self, idx: int, timeout: float = 1.0) -> bytes:
        if idx >= len(self._extra_clients) or not self._extra_clients[idx]:
            return b""
        child = self._extra_clients[idx]
        data = b""
        try:
            while True:
                chunk = child.read_nonblocking(size=65536, timeout=timeout)
                data += chunk
                timeout = 0.1
        except (pexpect.TIMEOUT, pexpect.EOF):
            pass
        return data

    def extra_client_alive(self, idx: int) -> bool:
        if idx >= len(self._extra_clients):
            return False
        child = self._extra_clients[idx]
        return child is not None and child.isalive()

    def send_extra_client(self, idx: int, keys: str) -> None:
        if idx < len(self._extra_clients) and self._extra_clients[idx]:
            self._extra_clients[idx].send(keys.encode() if isinstance(keys, str) else keys)

    def close(self) -> None:
        for child in self._extra_clients:
            if child and child.isalive():
                child.terminate(force=True)
        self._extra_clients.clear()
        self._tmux("kill-server", check=False)


class Driver:
    """Drives mini-tmux via pexpect (full PTY interaction)."""

    def __init__(
        self,
        binary: str = "tmux",
        server_name: Optional[str] = None,
        rows: int = 24,
        cols: int = 80,
    ) -> None:
        self._binary = binary
        self._server_name = server_name or f"harness_{os.getpid()}"
        self._rows = rows
        self._cols = cols
        self._child: Optional[pexpect.spawn] = None
        self._server_pid_cache: Optional[int] = None
        self._is_tmux = os.path.basename(binary) == "tmux"
        self._extra_clients: list[pexpect.spawn] = []

    def start(self) -> None:
        if self._is_tmux:
            cmd = (
                f"tmux -L {self._server_name} "
                f"new-session -x {self._cols} -y {self._rows}"
            )
        else:
            cmd = self._binary
        env = os.environ.copy()
        if not self._is_tmux:
            env["MINI_TMUX_SERVER"] = self._server_name
        self._child = pexpect.spawn(
            cmd,
            dimensions=(self._rows, self._cols),
            encoding=None,
            timeout=5,
            env=env,
        )
        self._child.delaybeforesend = 0.05
        self._server_pid_cache = None
        time.sleep(0.5)
        if self._is_tmux:
            for _ in range(20):
                r = subprocess.run(
                    ["tmux", "-L", self._server_name, "list-sessions"],
                    capture_output=True,
                )
                if r.returncode == 0:
                    break
                time.sleep(0.2)

    def server_pid(self) -> int:
        if self._is_tmux:
            result = subprocess.run(
                ["tmux", "-L", self._server_name, "display-message", "-p", "#{pid}"],
                capture_output=True, text=True,
            )
            if result.returncode == 0 and result.stdout.strip():
                pid = int(result.stdout.strip())
                self._server_pid_cache = pid
                return pid
            for proc in psutil.process_iter(["pid", "name", "cmdline"]):
                try:
                    cmdline = proc.info["cmdline"] or []
                    if "tmux" in " ".join(cmdline) and self._server_name in " ".join(cmdline) and "server" in " ".join(cmdline):
                        self._server_pid_cache = proc.pid
                        return proc.pid
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    continue
        else:
            if self._child is None:
                raise RuntimeError("Cannot find server PID")
            try:
                root = psutil.Process(self._child.pid)
            except psutil.NoSuchProcess:
                raise RuntimeError("Cannot find server PID")
            for proc in root.children(recursive=True):
                try:
                    cmdline = proc.cmdline()
                    if "--server" in cmdline:
                        self._server_pid_cache = proc.pid
                        return proc.pid
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    continue
        raise RuntimeError("Cannot find server PID")

    def server_alive(self) -> bool:
        if self._server_pid_cache:
            return psutil.pid_exists(self._server_pid_cache)
        try:
            self.server_pid()
            return True
        except RuntimeError:
            return False

    def client_alive(self) -> bool:
        return self._child is not None and self._child.isalive()

    def detach(self) -> None:
        if self._is_tmux:
            if not self._server_pid_cache:
                try:
                    self.server_pid()
                except RuntimeError:
                    pass
            self._child.send(b"\x02")
            time.sleep(0.2)
            self._child.send(b"d")
            try:
                self._child.expect(pexpect.EOF, timeout=5)
            except pexpect.TIMEOUT:
                pass
            time.sleep(0.2)

    def reattach(self) -> None:
        if self._is_tmux:
            cmd = f"tmux -L {self._server_name} attach"
        else:
            cmd = f"{self._binary} attach"
        env = os.environ.copy()
        if not self._is_tmux:
            env["MINI_TMUX_SERVER"] = self._server_name
        self._child = pexpect.spawn(
            cmd,
            dimensions=(self._rows, self._cols),
            encoding=None,
            timeout=5,
            env=env,
        )
        self._child.delaybeforesend = 0.05
        time.sleep(0.3)

    def send_keys(self, keys: str) -> None:
        if isinstance(keys, str):
            self._child.send(keys.encode())
        else:
            self._child.send(keys)

    def send_line(self, text: str) -> None:
        self._child.sendline(text.encode() if isinstance(text, str) else text)

    def read_raw(self, timeout: float = 1.0) -> bytes:
        data = b""
        if self._child.buffer:
            buf = self._child.buffer
            data += buf if isinstance(buf, bytes) else buf.encode()
            self._child.buffer = b""
        if hasattr(self._child, "before") and self._child.before:
            before = self._child.before
            data += before if isinstance(before, bytes) else before.encode()
        try:
            while True:
                chunk = self._child.read_nonblocking(size=65536, timeout=timeout)
                data += chunk
                timeout = 0.1
        except pexpect.TIMEOUT:
            pass
        except pexpect.EOF:
            pass
        return data

    def resize(self, rows: int, cols: int) -> None:
        self._rows = rows
        self._cols = cols
        if self._child:
            self._child.setwinsize(rows, cols)

    def attach_client(self, readonly: bool = False, rows: int = 0, cols: int = 0) -> int:
        r = rows or self._rows
        c = cols or self._cols
        if self._is_tmux:
            cmd = f"tmux -L {self._server_name} attach"
            if readonly:
                cmd += " -r"
        else:
            cmd = f"{self._binary} attach"
            if readonly:
                cmd += " -r"
        env = os.environ.copy()
        if not self._is_tmux:
            env["MINI_TMUX_SERVER"] = self._server_name
        child = pexpect.spawn(cmd, dimensions=(r, c), encoding=None, timeout=5, env=env)
        child.delaybeforesend = 0.05
        time.sleep(0.3)
        self._extra_clients.append(child)
        return len(self._extra_clients) - 1

    def new_session_client(self, rows: int = 0, cols: int = 0) -> int:
        r = rows or self._rows
        c = cols or self._cols
        env = os.environ.copy()
        if self._is_tmux:
            session_name = f"s{len(self._extra_clients) + 1}"
            cmd = f"tmux -L {self._server_name} new-session -s {session_name}"
        else:
            cmd = self._binary
            env["MINI_TMUX_SERVER"] = self._server_name
        child = pexpect.spawn(cmd, dimensions=(r, c), encoding=None, timeout=5, env=env)
        child.delaybeforesend = 0.05
        time.sleep(0.5)
        self._extra_clients.append(child)
        return len(self._extra_clients) - 1

    def detach_extra_client(self, idx: int) -> None:
        if idx < len(self._extra_clients) and self._extra_clients[idx]:
            child = self._extra_clients[idx]
            child.send(b"\x02")
            time.sleep(0.1)
            child.send(b"d")
            try:
                child.expect(pexpect.EOF, timeout=3)
            except pexpect.TIMEOUT:
                pass
            time.sleep(0.2)

    def kill_extra_client(self, idx: int) -> None:
        if idx < len(self._extra_clients) and self._extra_clients[idx]:
            self._extra_clients[idx].terminate(force=True)
            time.sleep(0.2)

    def read_extra_client(self, idx: int, timeout: float = 1.0) -> bytes:
        if idx >= len(self._extra_clients) or not self._extra_clients[idx]:
            return b""
        child = self._extra_clients[idx]
        data = b""
        try:
            while True:
                chunk = child.read_nonblocking(size=65536, timeout=timeout)
                data += chunk
                timeout = 0.1
        except (pexpect.TIMEOUT, pexpect.EOF):
            pass
        return data

    def extra_client_alive(self, idx: int) -> bool:
        if idx >= len(self._extra_clients):
            return False
        child = self._extra_clients[idx]
        return child is not None and child.isalive()

    def send_extra_client(self, idx: int, keys: str) -> None:
        if idx < len(self._extra_clients) and self._extra_clients[idx]:
            self._extra_clients[idx].send(keys.encode() if isinstance(keys, str) else keys)

    def close(self) -> None:
        for child in self._extra_clients:
            if child and child.isalive():
                child.terminate(force=True)
        self._extra_clients.clear()
        if self._child and self._child.isalive():
            self._child.terminate(force=True)
        if self._is_tmux:
            subprocess.run(
                ["tmux", "-L", self._server_name, "kill-server"],
                capture_output=True,
            )
        self._child = None
