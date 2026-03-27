"""Protocol adapters: command mode operations for mini-tmux and tmux."""
from __future__ import annotations

import subprocess
import time
from typing import Optional

from harness.driver import Driver


class MiniTmuxProtocol:
    """Encapsulates command mode operations for mini-tmux."""

    def __init__(self, driver: Driver) -> None:
        self._driver = driver
        self._pane_order: list[int] = [0]
        self._focus: int = 0
        self._next_pane_id: int = 1

    def create_pane(self) -> None:
        self._driver.send_line(":new")
        pane_id = self._next_pane_id
        self._next_pane_id += 1
        self._pane_order.append(pane_id)
        self._focus = pane_id
        time.sleep(0.3)

    def kill_pane(self, pane_id: int) -> None:
        self._driver.send_line(f":kill {pane_id}")
        if pane_id in self._pane_order:
            idx = self._pane_order.index(pane_id)
            self._pane_order.remove(pane_id)
            if self._focus == pane_id:
                if self._pane_order:
                    self._focus = self._pane_order[max(0, idx - 1)]
                else:
                    self._focus = 0
        time.sleep(0.3)

    def switch_focus(self, pane_id: int) -> None:
        self._driver.send_line(f":focus {pane_id}")
        self._focus = pane_id
        if pane_id not in self._pane_order:
            self._pane_order.append(pane_id)
            self._pane_order.sort()
        time.sleep(0.2)

    def send_keys_to_pane(self, pane_id: int, keys: str) -> None:
        self.switch_focus(pane_id)
        if keys == "C-c":
            self._driver.send_keys(b"\x03")
            return
        if keys == "C-z":
            self._driver.send_keys(b"\x1a")
            return
        if keys == "Enter":
            self._driver.send_keys(b"\r")
            return
        if keys.startswith("C-") and len(keys) == 3:
            ch = keys[-1].lower()
            code = ord(ch) - ord("a") + 1
            if 1 <= code <= 26:
                self._driver.send_keys(bytes([code]))
                return
        self._driver.send_keys(keys)

    def start_probe_in_pane(
        self,
        probe_path: str,
        sideband_path: str,
        session_id: str,
    ) -> None:
        self._driver.send_line(f"{probe_path} {sideband_path} {session_id}")
        time.sleep(0.3)


class TmuxProtocol(MiniTmuxProtocol):
    """tmux adapter: maps mini-tmux semantics to real tmux commands."""

    def __init__(self, driver: Driver) -> None:
        super().__init__(driver)
        self._pane_count = 1
        self._tmux_pane_ids: dict[int, str] = {}
        self._init_pane_id()

    @property
    def _server_name(self) -> str:
        return self._driver._server_name

    def _tmux(self, *args: str, check: bool = True) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["tmux", "-L", self._server_name, *args],
            capture_output=True, text=True, check=check,
        )

    def _init_pane_id(self) -> None:
        pass

    def _ensure_pane0(self) -> None:
        if 0 not in self._tmux_pane_ids:
            r = self._tmux("list-panes", "-F", "#{pane_id}", check=False)
            if r.returncode == 0 and r.stdout.strip():
                self._tmux_pane_ids[0] = r.stdout.strip().split("\n")[0]

    def _tmux_target(self, pane_id: int) -> str:
        self._ensure_pane0()
        tid = self._tmux_pane_ids.get(pane_id)
        if tid:
            return tid
        return str(pane_id)

    def create_pane(self) -> None:
        self._ensure_pane0()
        first_tid = self._tmux_pane_ids.get(0, "0")
        self._tmux("select-pane", "-t", first_tid, check=False)
        r = self._tmux("split-window", "-PF", "#{pane_id}", check=True)
        new_tmux_id = r.stdout.strip()
        self._pane_count += 1
        self._pending_tmux_id = new_tmux_id
        self._tmux("select-layout", "even-vertical", check=False)
        time.sleep(0.3)

    def _register_pane(self, logical_id: int) -> None:
        if hasattr(self, '_pending_tmux_id') and self._pending_tmux_id:
            self._tmux_pane_ids[logical_id] = self._pending_tmux_id
            self._pending_tmux_id = None

    def kill_pane(self, pane_id: int) -> None:
        tid = self._tmux_target(pane_id)
        self._tmux("kill-pane", "-t", tid, check=False)
        self._tmux_pane_ids.pop(pane_id, None)
        time.sleep(0.3)

    def switch_focus(self, pane_id: int) -> None:
        tid = self._tmux_target(pane_id)
        self._tmux("select-pane", "-t", tid, check=True)
        time.sleep(0.2)

    def start_probe_in_pane(
        self,
        probe_path: str,
        sideband_path: str,
        session_id: str,
    ) -> None:
        cmd = f"{probe_path} {sideband_path} {session_id}"
        self._tmux("send-keys", cmd, "Enter", check=True)
        time.sleep(0.3)

    def send_keys_to_pane(self, pane_id: int, keys: str) -> None:
        tid = self._tmux_target(pane_id)
        self._tmux("send-keys", "-t", tid, keys, check=True)
