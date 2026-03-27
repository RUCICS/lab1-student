"""Sideband server: Unix domain socket for receiving JSON messages from probe helpers."""
from __future__ import annotations

import json
import os
import selectors
import socket
import threading
from typing import Optional


class SidebandServer:
    """Accepts connections from probe helpers and collects JSON messages.

    Each probe connects, sends newline-delimited JSON messages, then disconnects.
    Thread-safe: the accept loop runs in a background thread.
    """

    def __init__(self, socket_path: str) -> None:
        self._socket_path = socket_path
        self._messages: list[dict] = []
        self._lock = threading.Lock()
        self._event = threading.Event()
        self._server_sock: Optional[socket.socket] = None
        self._thread: Optional[threading.Thread] = None
        self._running = False

    @property
    def socket_path(self) -> str:
        return self._socket_path

    def start(self) -> None:
        if os.path.exists(self._socket_path):
            os.unlink(self._socket_path)
        self._server_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._server_sock.bind(self._socket_path)
        self._server_sock.listen(32)
        self._server_sock.setblocking(False)
        self._running = True
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()

    def _accept_loop(self) -> None:
        sel = selectors.DefaultSelector()
        sel.register(self._server_sock, selectors.EVENT_READ)
        buffers: dict[int, bytes] = {}

        while self._running:
            events = sel.select(timeout=0.1)
            for key, mask in events:
                if key.fileobj is self._server_sock:
                    try:
                        conn, _ = self._server_sock.accept()
                        conn.setblocking(False)
                        sel.register(conn, selectors.EVENT_READ)
                        buffers[conn.fileno()] = b""
                    except OSError:
                        continue
                else:
                    conn = key.fileobj
                    try:
                        data = conn.recv(65536)
                    except BlockingIOError:
                        continue
                    except OSError:
                        data = b""
                    if not data:
                        fd = conn.fileno()
                        remaining = buffers.pop(fd, b"")
                        if remaining.strip():
                            self._parse_and_store(remaining)
                        sel.unregister(conn)
                        conn.close()
                        continue
                    fd = conn.fileno()
                    buffers[fd] = buffers.get(fd, b"") + data
                    while b"\n" in buffers[fd]:
                        line, buffers[fd] = buffers[fd].split(b"\n", 1)
                        if line.strip():
                            self._parse_and_store(line)

        for key in list(sel.get_map().values()):
            if key.fileobj is not self._server_sock:
                sel.unregister(key.fileobj)
                key.fileobj.close()
        sel.unregister(self._server_sock)
        sel.close()

    def _parse_and_store(self, data: bytes) -> None:
        try:
            msg = json.loads(data)
            with self._lock:
                self._messages.append(msg)
                self._event.set()
        except (json.JSONDecodeError, UnicodeDecodeError):
            pass

    def wait_message(self, timeout: float = 5.0) -> dict:
        import time as _time
        end = _time.monotonic() + timeout
        with self._lock:
            start = len(self._messages)
        while True:
            with self._lock:
                if len(self._messages) > start:
                    return self._messages[start]
                self._event.clear()
            remaining = end - _time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"No sideband message received within {timeout}s")
            self._event.wait(timeout=remaining)
            with self._lock:
                if len(self._messages) > start:
                    return self._messages[start]

    def get_messages(
        self,
        session_id: Optional[str] = None,
        msg_type: Optional[str] = None,
    ) -> list[dict]:
        with self._lock:
            msgs = list(self._messages)
        if session_id is not None:
            msgs = [m for m in msgs if m.get("session") == session_id]
        if msg_type is not None:
            msgs = [m for m in msgs if m.get("type") == msg_type]
        return msgs

    def clear(self) -> None:
        with self._lock:
            self._messages.clear()

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=3.0)
        if self._server_sock:
            try:
                self._server_sock.close()
            except OSError:
                pass
        if os.path.exists(self._socket_path):
            os.unlink(self._socket_path)
