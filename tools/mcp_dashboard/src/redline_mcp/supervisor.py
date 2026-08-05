"""Launches and tracks the dashboard and editor processes.

At most one instance of each application type runs at a time. That is a
deliberate limit, not an oversight: instances share a zenoh bus, so a second
dashboard would observe samples injected at the first, and telling the two apart
would need per-instance zenoh connectivity configuration for very little gain.
"""

from __future__ import annotations

import atexit
import collections
import os
import pathlib
import signal
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass, field
from typing import Literal

from .client import AgentClient

AppName = Literal["dashboard", "editor", "scope"]

# How long to wait for the AGENT_READY handshake before giving up on a launch.
# Generous because a debug build loading a large config on a cold page cache is
# genuinely slow.
READY_TIMEOUT_S = 30.0

# Lines of the child's stderr kept for crash reporting. The app's in-process log
# ring dies with the process, so this is what survives a segfault.
STDERR_RING = 400


class LaunchError(RuntimeError):
    pass


@dataclass
class Instance:
    app: AppName
    process: subprocess.Popen
    socket_path: str
    config_path: str | None
    client: AgentClient
    started_at: float
    stderr: collections.deque[str] = field(default_factory=lambda: collections.deque(maxlen=STDERR_RING))

    def alive(self) -> bool:
        return self.process.poll() is None

    def stderr_tail(self, lines: int = 40) -> str:
        return "".join(list(self.stderr)[-lines:])


def _repo_root() -> pathlib.Path:
    """Locate the repository root.

    Walks up looking for a marker rather than counting parent directories. A
    fixed count encodes the current layout in a number: move this package one
    level and it silently resolves to the wrong directory, then fails much later
    as a confusing "no dashboard binary" error rather than at the cause.

    REDLINE_REPO_ROOT overrides it, which is the escape hatch for running the
    server from outside the tree (installed non-editable, say, where __file__ is
    in site-packages and no marker is above it).
    """
    override = os.environ.get("REDLINE_REPO_ROOT")
    if override:
        return pathlib.Path(override).expanduser().resolve()

    here = pathlib.Path(__file__).resolve()
    for candidate in here.parents:
        # Both markers, so a nested git repo or a stray CMakeLists cannot match.
        if (candidate / ".git").exists() and (candidate / "CMakeLists.txt").is_file():
            return candidate

    raise LaunchError(
        f"Could not find the repository root above {here}. "
        f"Set REDLINE_REPO_ROOT to the checkout directory."
    )


def binary_for(app: AppName) -> pathlib.Path:
    root = _repo_root()
    build = pathlib.Path(os.environ.get("REDLINE_BUILD_DIR", root / "build"))

    if app == "dashboard":
        candidates = [build / "dashboard" / "dashboard"]
    elif app == "scope":
        # Top-level target, and a plain executable everywhere: only the editor
        # is built as a macOS bundle.
        candidates = [build / "scope" / "scope"]
    else:
        # The editor is a macOS bundle in this tree; plain executable elsewhere.
        candidates = [
            build / "dashboard" / "editor.app" / "Contents" / "MacOS" / "editor",
            build / "dashboard" / "editor",
        ]

    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate

    raise LaunchError(
        f"No {app} binary found. Looked in: {', '.join(str(c) for c in candidates)}. "
        f"Build it first: cmake --build {build} --target {app}"
    )


class Supervisor:
    def __init__(self) -> None:
        self._instances: dict[str, Instance] = {}
        self._lock = threading.Lock()
        self._tmpdir = tempfile.mkdtemp(prefix="redline-mcp-")
        atexit.register(self.shutdown)

    # ------------------------------------------------------------------ launch

    def launch(
        self,
        app: AppName,
        config: str | None = None,
        extra_args: list[str] | None = None,
    ) -> Instance:
        with self._lock:
            existing = self._instances.get(app)
            if existing is not None and existing.alive():
                # Replace rather than error: relaunching to pick up a rebuild or
                # a different config is the common case, and refusing would just
                # mean every caller writes quit-then-launch by hand.
                self._quit_locked(app)

            binary = binary_for(app)
            socket_path = os.path.join(self._tmpdir, f"{app}.sock")
            if os.path.exists(socket_path):
                os.unlink(socket_path)

            argv = [str(binary), f"--mcp={socket_path}"]
            if config:
                argv += ["-c", config]
            argv += extra_args or []

            process = subprocess.Popen(
                argv,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
                cwd=str(_repo_root()),
                # Own process group, so shutdown can take down anything the app
                # spawned rather than orphaning it.
                start_new_session=True,
            )

            instance = Instance(
                app=app,
                process=process,
                socket_path=socket_path,
                config_path=config,
                client=AgentClient(socket_path),
                started_at=time.time(),
            )

            ready = threading.Event()

            def pump_stderr() -> None:
                assert process.stderr is not None
                for line in process.stderr:
                    instance.stderr.append(line)

            def pump_stdout() -> None:
                assert process.stdout is not None
                for line in process.stdout:
                    if line.startswith("AGENT_READY"):
                        ready.set()

            stderr_thread = threading.Thread(target=pump_stderr, daemon=True)
            stderr_thread.start()
            threading.Thread(target=pump_stdout, daemon=True).start()

            def drained_tail() -> str:
                # The pump thread ends at EOF, which arrives once the process is
                # gone. Join it before reading so the tail is complete: an app
                # that dies during startup writes its reason and exits in the
                # same breath, so without this there is a race between the pump
                # and the report -- and an error report that loses the error is
                # worse than no report.
                stderr_thread.join(timeout=2.0)
                return instance.stderr_tail()

            # Wait for the handshake, but stop early if the process dies -- that
            # turns a crash-on-start into an immediate, explained failure instead
            # of a 30-second timeout that says nothing.
            deadline = time.time() + READY_TIMEOUT_S
            while time.time() < deadline:
                if ready.wait(0.1):
                    break
                if process.poll() is not None:
                    raise LaunchError(
                        f"{app} exited with code {process.returncode} before it was ready.\n"
                        f"--- stderr ---\n{drained_tail()}"
                    )
            else:
                process.kill()
                raise LaunchError(
                    f"{app} did not report AGENT_READY within {READY_TIMEOUT_S:g}s.\n"
                    f"--- stderr ---\n{drained_tail()}"
                )

            self._instances[app] = instance
            return instance

    # ------------------------------------------------------------------ access

    def get(self, app: AppName | None) -> Instance:
        with self._lock:
            live = {name: inst for name, inst in self._instances.items() if inst.alive()}

            if app is None:
                if not live:
                    raise LaunchError(
                        "No application is running. Call app_launch first."
                    )
                if len(live) > 1:
                    names = ", ".join(sorted(live))
                    raise LaunchError(
                        f"Both {names} are running; pass app= to say which one you mean."
                    )
                return next(iter(live.values()))

            instance = self._instances.get(app)
            if instance is None:
                raise LaunchError(f"No {app} is running. Call app_launch(app='{app}') first.")
            if not instance.alive():
                raise LaunchError(
                    f"The {app} process exited with code {instance.process.returncode}.\n"
                    f"--- stderr ---\n{instance.stderr_tail()}"
                )
            return instance

    def list(self) -> list[dict[str, object]]:
        with self._lock:
            out = []
            for name, inst in sorted(self._instances.items()):
                out.append(
                    {
                        "app": name,
                        "pid": inst.process.pid,
                        "running": inst.alive(),
                        "exit_code": inst.process.returncode,
                        "config_path": inst.config_path,
                        "socket_path": inst.socket_path,
                        "uptime_s": round(time.time() - inst.started_at, 1),
                    }
                )
            return out

    # ---------------------------------------------------------------- teardown

    def quit(self, app: AppName) -> bool:
        with self._lock:
            return self._quit_locked(app)

    def _quit_locked(self, app: AppName) -> bool:
        instance = self._instances.pop(app, None)
        if instance is None:
            return False

        instance.client.close()
        if instance.alive():
            try:
                # Signal the group: start_new_session made the child a leader, so
                # this reaches anything it spawned too.
                os.killpg(os.getpgid(instance.process.pid), signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                instance.process.terminate()
            try:
                instance.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(os.getpgid(instance.process.pid), signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    instance.process.kill()
                instance.process.wait(timeout=5)
        return True

    def shutdown(self) -> None:
        for app in list(self._instances):
            try:
                self.quit(app)  # type: ignore[arg-type]
            except Exception:
                pass
