"""Verifier: reads sideband messages from probe helpers and judges pass/fail."""
from __future__ import annotations

import os
import secrets
from dataclasses import dataclass, field
from typing import Any, Optional

import psutil

from harness.driver import Driver
from harness.sideband import SidebandServer


@dataclass
class Result:
    passed: bool
    message: str = ""
    data: dict[str, Any] = field(default_factory=dict)


class Verifier:
    """Aggregates probe sideband reports and makes pass/fail judgments."""

    def __init__(self, sideband: SidebandServer, driver: Driver) -> None:
        self._sb = sideband
        self._driver = driver

    def verify_env_check(
        self,
        session_id: str,
        expect_isatty: bool = True,
        expect_rows: Optional[int] = None,
        expect_cols: Optional[int] = None,
    ) -> Result:
        msgs = self._sb.get_messages(session_id=session_id, msg_type="env_check")
        if not msgs:
            return Result(False, f"No env_check message from session {session_id}")
        msg = msgs[0]
        issues = []
        if expect_isatty and not msg.get("isatty_stdin"):
            issues.append("isatty_stdin is False")
        if expect_isatty and not msg.get("isatty_stdout"):
            issues.append("isatty_stdout is False")
        ws = msg.get("winsize", {})
        if expect_rows and ws.get("rows") != expect_rows:
            issues.append(f"rows: expected {expect_rows}, got {ws.get('rows')}")
        if expect_cols and ws.get("cols") != expect_cols:
            issues.append(f"cols: expected {expect_cols}, got {ws.get('cols')}")
        if issues:
            return Result(False, "; ".join(issues), data=msg)
        return Result(True, "env_check OK", data=msg)

    def verify_pgrp_isolation(self, sessions: list[str]) -> Result:
        pgrps = {}
        for sid in sessions:
            msgs = self._sb.get_messages(session_id=sid, msg_type="env_check")
            if not msgs:
                return Result(False, f"No env_check from {sid}")
            pgrps[sid] = msgs[0].get("pgrp")
        values = list(pgrps.values())
        if len(set(values)) != len(values):
            return Result(False, f"Process groups not isolated: {pgrps}", data=pgrps)
        return Result(True, "pgrp isolation OK", data=pgrps)

    def verify_signal_delivery(
        self,
        session_id: str,
        signal_name: str,
    ) -> Result:
        msgs = self._sb.get_messages(session_id=session_id, msg_type="signal")
        matching = [m for m in msgs if m.get("signal") == signal_name]
        if not matching:
            return Result(
                False,
                f"No {signal_name} received by {session_id}",
                data={"all_signals": msgs},
            )
        return Result(True, f"{signal_name} received by {session_id}", data=matching[0])

    def verify_signal_isolation(
        self,
        active: str,
        inactive: list[str],
        signal: str,
    ) -> Result:
        active_result = self.verify_signal_delivery(active, signal)
        if not active_result.passed:
            return Result(False, f"Active pane {active} did not receive {signal}")
        for sid in inactive:
            msgs = self._sb.get_messages(session_id=sid, msg_type="signal")
            matching = [m for m in msgs if m.get("signal") == signal]
            if matching:
                return Result(
                    False,
                    f"Inactive pane {sid} received {signal} (signal leak)",
                    data={"leaked_to": sid},
                )
        return Result(True, f"{signal} isolation OK: {active} got it, {inactive} did not")

    def verify_output_token(self, session_id: str) -> Result:
        msgs = self._sb.get_messages(session_id=session_id, msg_type="output_token")
        if not msgs:
            return Result(False, f"No output_token from {session_id}")
        token = msgs[0]["token"]
        raw = self._driver.read_raw(timeout=2.0)
        if token.encode() in raw:
            return Result(True, "output_token passthrough OK", data={"token": token})
        return Result(
            False,
            f"Token {token} not found in client output ({len(raw)} bytes read)",
            data={"token": token, "raw_len": len(raw)},
        )

    def verify_input_token(self, session_id: str) -> Result:
        token = secrets.token_hex(16)
        self._driver.send_line(token)

        import time
        time.sleep(1.0)

        msgs = self._sb.get_messages(session_id=session_id, msg_type="input_token")
        matching = [m for m in msgs if m.get("received") == token]
        if matching:
            return Result(True, "input_token passthrough OK", data={"token": token})
        return Result(
            False,
            f"Probe did not report receiving token {token}",
            data={"sent": token, "received_msgs": msgs},
        )
