"""Process tree inspection and zombie checking using psutil."""
from __future__ import annotations

from typing import Optional

import psutil


class ProcessTree:
    """Inspect the process tree rooted at a given PID."""

    def __init__(self, root_pid: int) -> None:
        self._root_pid = root_pid

    def _root(self) -> Optional[psutil.Process]:
        try:
            return psutil.Process(self._root_pid)
        except psutil.NoSuchProcess:
            return None

    def children(self, recursive: bool = False) -> list[psutil.Process]:
        root = self._root()
        if not root:
            return []
        try:
            return root.children(recursive=recursive)
        except psutil.NoSuchProcess:
            return []

    def all_descendants(self) -> list[psutil.Process]:
        return self.children(recursive=True)

    def zombie_count(self) -> int:
        count = 0
        for proc in self.all_descendants():
            try:
                if proc.status() == psutil.STATUS_ZOMBIE:
                    count += 1
            except psutil.NoSuchProcess:
                continue
        return count

    def find_by_cmdline(self, pattern: str) -> list[psutil.Process]:
        results = []
        for proc in self.all_descendants():
            try:
                cmdline = " ".join(proc.cmdline())
                if pattern in cmdline:
                    results.append(proc)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
        return results

    def alive(self) -> bool:
        root = self._root()
        if not root:
            return False
        try:
            return root.is_running()
        except psutil.NoSuchProcess:
            return False
