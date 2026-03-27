"""Runner: YAML workload parser + step executor (basic tests only)."""
from __future__ import annotations

import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import yaml

from harness.driver import TmuxDriver, Driver
from harness.protocol import TmuxProtocol, MiniTmuxProtocol
from harness.sideband import SidebandServer
from harness.verifier import Verifier, Result
from harness.process_tree import ProcessTree

import secrets
import subprocess
import tempfile

HELPERS_DIR = Path(__file__).parent / "helpers"
PROBE_BIN = str(HELPERS_DIR / "probe")


@dataclass
class WorkloadResult:
    workload_id: str
    workload_name: str
    passed: bool
    steps: list[StepResult] = field(default_factory=list)
    error: Optional[str] = None


@dataclass
class StepResult:
    action: str
    passed: bool
    message: str = ""
    data: dict[str, Any] = field(default_factory=dict)


def load_workload(path: str | Path) -> dict:
    """Load and validate a YAML workload file."""
    with open(path) as f:
        data = yaml.safe_load(f)
    _validate_workload(data)
    return data


def _validate_workload(data: dict) -> None:
    required = ["name", "id", "steps"]
    for key in required:
        if key not in data:
            raise ValueError(f"Workload missing required field: {key}")
    if not isinstance(data["steps"], list) or len(data["steps"]) == 0:
        raise ValueError("Workload must have at least one step")
    for i, step in enumerate(data["steps"]):
        if "action" not in step:
            raise ValueError(f"Step {i} missing 'action' field")


class Runner:
    """Executes a YAML workload against tmux (for validation) or mini-tmux.

    This runner only supports actions needed for basic tests. Workloads that
    use unsupported actions (resize, log, pipeout, capture, layout, etc.)
    will fail at the unsupported step.
    """

    def __init__(
        self,
        binary: Optional[str] = None,
        use_tmux: bool = True,
    ) -> None:
        self._binary = binary
        self._use_tmux = use_tmux
        self._driver: Optional[TmuxDriver | Driver] = None
        self._protocol: Optional[TmuxProtocol | MiniTmuxProtocol] = None
        self._sideband: Optional[SidebandServer] = None
        self._verifier: Optional[Verifier] = None
        self._sock_path: Optional[str] = None
        self._server_name: Optional[str] = None
        self._pane_sessions: dict[int, str] = {}
        self._next_pane: int = 0
        self._tmp_dir: Optional[str] = None
        self._extra_client_ids: list[int] = []
        self._session_client_map: dict[int, int] = {}

    def supported_actions(self) -> set[str]:
        """Return the set of actions this runner supports."""
        return {
            name.removeprefix("_step_")
            for name in dir(self)
            if name.startswith("_step_")
        }

    def can_run(self, workload: dict) -> bool:
        """Check if all actions in a workload are supported."""
        supported = self.supported_actions()
        return all(step["action"] in supported for step in workload["steps"])

    def run_workload(self, workload: dict) -> WorkloadResult:
        wid = workload["id"]
        wname = workload["name"]
        result = WorkloadResult(workload_id=wid, workload_name=wname, passed=True)
        timeout_sec = workload.get("timeout_sec", 30)
        terminal = workload.get("terminal", {})
        rows = terminal.get("rows", 24)
        cols = terminal.get("cols", 80)

        if workload.get("skip_tmux") and self._use_tmux:
            result.passed = True
            result.steps.append(StepResult(action="skip", passed=True, message="skipped for tmux"))
            return result

        try:
            self._setup(rows, cols)
            start_time = time.monotonic()
            for step in workload["steps"]:
                if time.monotonic() - start_time > timeout_sec:
                    result.passed = False
                    result.error = "Workload timeout"
                    break
                sr = self._execute_step(step)
                result.steps.append(sr)
                if not sr.passed:
                    result.passed = False
                    break
        except Exception as e:
            result.passed = False
            result.error = str(e)
        finally:
            self._teardown()

        return result

    def _setup(self, rows: int, cols: int) -> None:
        import uuid
        self._server_name = f"hr_{uuid.uuid4().hex[:12]}"
        self._sock_path = f"/tmp/hr_{uuid.uuid4().hex[:8]}.sock"
        self._tmp_dir = tempfile.mkdtemp(prefix="hr_workload_")

        self._sideband = SidebandServer(self._sock_path)
        self._sideband.start()

        if self._use_tmux:
            self._driver = TmuxDriver(server_name=self._server_name, rows=rows, cols=cols)
            self._protocol = TmuxProtocol(self._driver)
        else:
            self._driver = Driver(binary=self._binary, server_name=self._server_name, rows=rows, cols=cols)
            self._protocol = MiniTmuxProtocol(self._driver)

        self._verifier = Verifier(self._sideband, self._driver)
        self._pane_sessions.clear()
        self._next_pane = 0

    def _teardown(self) -> None:
        if self._driver:
            self._driver.close()
        if self._sideband:
            self._sideband.stop()
        if self._sock_path and os.path.exists(self._sock_path):
            os.unlink(self._sock_path)
        if self._tmp_dir and os.path.exists(self._tmp_dir):
            import shutil
            shutil.rmtree(self._tmp_dir, ignore_errors=True)

    def _expand_vars(self, step: dict) -> dict:
        result = {}
        for k, v in step.items():
            if isinstance(v, str) and "$TMP" in v:
                result[k] = v.replace("$TMP", self._tmp_dir)
            else:
                result[k] = v
        return result

    def _execute_step(self, step: dict) -> StepResult:
        step = self._expand_vars(step)
        action = step["action"]
        handler = getattr(self, f"_step_{action}", None)
        if handler is None:
            return StepResult(action=action, passed=False, message=f"Unknown action: {action}")
        try:
            return handler(step)
        except Exception as e:
            return StepResult(action=action, passed=False, message=str(e))

    # --- Basic step handlers ---

    def _step_start(self, step: dict) -> StepResult:
        self._driver.start()
        time.sleep(0.5)
        return StepResult(action="start", passed=True, message="Server started")

    def _step_create_pane_with_probe(self, step: dict) -> StepResult:
        pane_id = self._next_pane
        session_id = step.get("session", f"pane_{pane_id}")

        if pane_id > 0:
            self._protocol.create_pane()
            if hasattr(self._protocol, '_register_pane'):
                self._protocol._register_pane(pane_id)
            time.sleep(0.3)

        self._protocol.start_probe_in_pane(PROBE_BIN, self._sock_path, session_id)
        self._pane_sessions[pane_id] = session_id
        self._next_pane += 1
        time.sleep(0.5)
        return StepResult(action="create_pane_with_probe", passed=True,
                         data={"pane_id": pane_id, "session_id": session_id})

    def _step_create_session_with_probe(self, step: dict) -> StepResult:
        pane_id = self._next_pane
        session_id = step.get("session", f"session_{pane_id}")

        idx = self._driver.new_session_client()
        self._extra_client_ids.append(idx)
        self._session_client_map[pane_id] = idx
        time.sleep(0.5)

        probe_cmd = f"{PROBE_BIN} {self._sock_path} {session_id}"
        self._driver.send_extra_client(idx, probe_cmd.encode() + b"\n")

        self._pane_sessions[pane_id] = session_id
        self._next_pane += 1
        time.sleep(0.5)
        return StepResult(action="create_session_with_probe", passed=True,
                         data={"pane_id": pane_id, "session_id": session_id})

    def _step_create_session(self, step: dict) -> StepResult:
        pane_id = self._next_pane
        idx = self._driver.new_session_client()
        self._extra_client_ids.append(idx)
        self._session_client_map[pane_id] = idx
        self._next_pane += 1
        time.sleep(0.3)
        return StepResult(action="create_session", passed=True,
                         data={"pane_id": pane_id})

    def _step_kill_session(self, step: dict) -> StepResult:
        pane = step["pane"]
        if pane in self._session_client_map:
            idx = self._session_client_map[pane]
            self._driver.kill_extra_client(idx)
            del self._session_client_map[pane]
        time.sleep(0.3)
        return StepResult(action="kill_session", passed=True)

    def _step_wait_probe_ready(self, step: dict) -> StepResult:
        pane = step.get("pane", 0)
        session_id = self._pane_sessions.get(pane, f"pane_{pane}")
        for _ in range(20):
            msgs = self._sideband.get_messages(session_id=session_id, msg_type="ready")
            if msgs:
                return StepResult(action="wait_probe_ready", passed=True)
            time.sleep(0.2)
        return StepResult(action="wait_probe_ready", passed=False,
                         message=f"Probe in pane {pane} not ready")

    def _step_verify_env_check(self, step: dict) -> StepResult:
        pane = step.get("pane", 0)
        session_id = self._pane_sessions.get(pane, f"pane_{pane}")
        r = self._verifier.verify_env_check(session_id)
        return StepResult(action="verify_env_check", passed=r.passed,
                         message=r.message, data=r.data)

    def _step_verify_pgrp_isolation(self, step: dict) -> StepResult:
        panes = step.get("panes", list(self._pane_sessions.keys()))
        sessions = [self._pane_sessions[p] for p in panes]
        r = self._verifier.verify_pgrp_isolation(sessions)
        return StepResult(action="verify_pgrp_isolation", passed=r.passed,
                         message=r.message, data=r.data)

    def _step_switch_focus(self, step: dict) -> StepResult:
        pane = step["pane"]
        self._protocol.switch_focus(pane)
        time.sleep(0.2)
        return StepResult(action="switch_focus", passed=True)

    @staticmethod
    def _resolve_keys(keys: str) -> bytes:
        if keys == "C-c":
            return b"\x03"
        if keys == "C-z":
            return b"\x1a"
        if keys == "Enter":
            return b"\r"
        if keys.startswith("C-") and len(keys) == 3:
            ch = keys[-1].lower()
            code = ord(ch) - ord("a") + 1
            if 1 <= code <= 26:
                return bytes([code])
        return keys.encode() if isinstance(keys, str) else keys

    def _step_send_keys(self, step: dict) -> StepResult:
        keys = step["keys"]
        pane = step.get("pane")
        if pane is not None and pane in self._session_client_map:
            idx = self._session_client_map[pane]
            self._driver.send_extra_client(idx, self._resolve_keys(keys))
        elif pane is not None and hasattr(self._protocol, "send_keys_to_pane"):
            self._protocol.send_keys_to_pane(pane, keys)
        else:
            self._driver.send_keys(keys)
        return StepResult(action="send_keys", passed=True)

    def _step_wait(self, step: dict) -> StepResult:
        ms = step.get("duration_ms", 500)
        time.sleep(ms / 1000.0)
        return StepResult(action="wait", passed=True)

    def _step_verify_signal(self, step: dict) -> StepResult:
        pane = step.get("pane", 0)
        signal = step["signal"]
        expect = step.get("expect", "received")
        session_id = self._pane_sessions.get(pane, f"pane_{pane}")
        r = self._verifier.verify_signal_delivery(session_id, signal)
        if expect == "received":
            return StepResult(action="verify_signal", passed=r.passed,
                             message=r.message, data=r.data)
        else:
            return StepResult(action="verify_signal", passed=not r.passed,
                             message=f"Expected no {signal}: {'leak!' if r.passed else 'OK'}")

    def _step_verify_output_token(self, step: dict) -> StepResult:
        pane = step.get("pane", 0)
        session_id = self._pane_sessions.get(pane, f"pane_{pane}")
        r = self._verifier.verify_output_token(session_id)
        return StepResult(action="verify_output_token", passed=r.passed,
                         message=r.message, data=r.data)

    def _step_verify_input_token(self, step: dict) -> StepResult:
        pane = step.get("pane", 0)
        session_id = self._pane_sessions.get(pane, f"pane_{pane}")

        if pane in self._session_client_map:
            idx = self._session_client_map[pane]
            token = secrets.token_hex(16)
            self._driver.send_extra_client(idx, token.encode() + b"\n")
            time.sleep(1.0)
            msgs = self._sideband.get_messages(session_id=session_id, msg_type="input_token")
            matching = [m for m in msgs if m.get("received") == token]
            if matching:
                return StepResult(action="verify_input_token", passed=True,
                                 message="input_token passthrough OK")
            return StepResult(action="verify_input_token", passed=False,
                             message="Probe did not report receiving token")
        else:
            self._protocol.switch_focus(pane)
            r = self._verifier.verify_input_token(session_id)
            return StepResult(action="verify_input_token", passed=r.passed,
                             message=r.message, data=r.data)

    def _step_verify_zombie_count(self, step: dict) -> StepResult:
        expect_max = step.get("max_zombies", 0)
        server_pid = self._driver.server_pid()
        pt = ProcessTree(server_pid)
        count = pt.zombie_count()
        passed = count <= expect_max
        return StepResult(
            action="verify_zombie_count", passed=passed,
            message=f"Zombies: {count} (max {expect_max})",
            data={"zombie_count": count},
        )

    def _step_create_pane(self, step: dict) -> StepResult:
        pane_id = self._next_pane
        self._protocol.create_pane()
        if hasattr(self._protocol, '_register_pane'):
            self._protocol._register_pane(pane_id)
        self._next_pane += 1
        return StepResult(action="create_pane", passed=True)

    def _step_run_command(self, step: dict) -> StepResult:
        cmd = step["command"]
        self._driver.send_line(cmd)
        time.sleep(step.get("delay_ms", 300) / 1000.0)
        return StepResult(action="run_command", passed=True)

    def _step_detach(self, step: dict) -> StepResult:
        self._driver.detach()
        time.sleep(0.5)
        return StepResult(action="detach", passed=True)

    def _step_reattach(self, step: dict) -> StepResult:
        self._driver.reattach()
        time.sleep(0.5)
        return StepResult(action="reattach", passed=True)

    def _step_verify_server_alive(self, step: dict) -> StepResult:
        alive = self._driver.server_alive()
        return StepResult(action="verify_server_alive", passed=alive,
                         message="alive" if alive else "server dead")

    def _step_verify_pane_alive(self, step: dict) -> StepResult:
        pane = step["pane"]
        session_id = self._pane_sessions.get(pane, f"pane_{pane}")
        msgs = self._sideband.get_messages(session_id=session_id, msg_type="output_token")
        passed = len(msgs) > 0
        return StepResult(action="verify_pane_alive", passed=passed,
                         message=f"pane {pane}: {'alive' if passed else 'no tokens'}")

    def _step_verify_client_exited(self, step: dict) -> StepResult:
        alive = self._driver.client_alive()
        return StepResult(action="verify_client_exited", passed=not alive,
                         message="client exited" if not alive else "client still alive")

    def _step_verify_server_dead(self, step: dict) -> StepResult:
        alive = self._driver.server_alive()
        return StepResult(action="verify_server_dead", passed=not alive,
                         message="server dead" if not alive else "server still alive")

    # --- Multi-client step handlers ---

    def _step_attach_client(self, step: dict) -> StepResult:
        readonly = step.get("readonly", False)
        rows = step.get("rows", 0)
        cols = step.get("cols", 0)
        idx = self._driver.attach_client(readonly=readonly, rows=rows, cols=cols)
        self._extra_client_ids.append(idx)
        time.sleep(0.5)
        return StepResult(action="attach_client", passed=True,
                         data={"client_idx": idx, "readonly": readonly})

    def _step_detach_extra_client(self, step: dict) -> StepResult:
        client = step.get("client", 0)
        if client < len(self._extra_client_ids):
            self._driver.detach_extra_client(self._extra_client_ids[client])
        return StepResult(action="detach_extra_client", passed=True)

    def _step_verify_extra_client_output(self, step: dict) -> StepResult:
        client = step.get("client", 0)
        pane = step.get("pane", 0)
        session_id = self._pane_sessions.get(pane, f"pane_{pane}")
        if client >= len(self._extra_client_ids):
            return StepResult(action="verify_extra_client_output", passed=False,
                             message=f"No extra client {client}")
        idx = self._extra_client_ids[client]
        msgs = self._sideband.get_messages(session_id=session_id, msg_type="output_token")
        if not msgs:
            return StepResult(action="verify_extra_client_output", passed=False,
                             message="No output tokens from probe")
        token = msgs[-1]["token"]
        raw = self._driver.read_extra_client(idx, timeout=2.0)
        if token.encode() in raw:
            return StepResult(action="verify_extra_client_output", passed=True,
                             message="Extra client sees probe output")
        return StepResult(action="verify_extra_client_output", passed=False,
                         message=f"Token not found in extra client output ({len(raw)} bytes)")

    def _step_verify_extra_client_alive(self, step: dict) -> StepResult:
        client = step.get("client", 0)
        if client >= len(self._extra_client_ids):
            return StepResult(action="verify_extra_client_alive", passed=False,
                             message=f"No extra client {client}")
        alive = self._driver.extra_client_alive(self._extra_client_ids[client])
        return StepResult(action="verify_extra_client_alive", passed=alive,
                         message="alive" if alive else "dead")

    def _step_verify_readonly_blocked(self, step: dict) -> StepResult:
        client = step.get("client", 0)
        pane = step.get("pane", 0)
        session_id = self._pane_sessions.get(pane, f"pane_{pane}")
        if client >= len(self._extra_client_ids):
            return StepResult(action="verify_readonly_blocked", passed=False,
                             message=f"No extra client {client}")
        idx = self._extra_client_ids[client]
        token = secrets.token_hex(16)
        self._driver.send_extra_client(idx, token + "\n")
        time.sleep(1.0)
        msgs = self._sideband.get_messages(session_id=session_id, msg_type="input_token")
        matching = [m for m in msgs if m.get("received") == token]
        if matching:
            return StepResult(action="verify_readonly_blocked", passed=False,
                             message="Readonly client input was NOT blocked")
        return StepResult(action="verify_readonly_blocked", passed=True,
                         message="Readonly client input correctly blocked")
