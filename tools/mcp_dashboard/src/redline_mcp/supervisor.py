"""Launches and tracks the processes this interface drives.

Two kinds, and the difference matters at every call site:

  * CONTROLLABLE apps -- dashboard, editor, scope -- embed libs/agent_control,
    take --mcp=<socket>, and answer JSON-RPC. Everything in the interface that
    inspects or clicks anything talks to one of these.
  * NODES -- map_server -- are headless and have no control socket. They are
    supervised only: launched, watched, read back through their output, and
    stopped. Embedding agent_control in one would mean linking Qt Widgets and a
    GUI-thread dispatcher into a server process, which is the wrong shape for
    an inarguable gain of nothing.

They are here together because the useful thing is running them together: a map
widget with no map_server behind it draws its background and says so, which is
a screenshot nobody wanted.

At most one instance of each type runs at a time. That is a deliberate limit,
not an oversight: instances share a zenoh bus, so a second dashboard would
observe samples injected at the first, and telling the two apart would need
per-instance zenoh connectivity configuration for very little gain.
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

AppName = Literal["dashboard", "editor", "scope", "map_server", "bd992_mock"]


@dataclass(frozen=True)
class AppSpec:
    """How to start one process and how to know it came up.

    A table rather than a chain of `if app ==`, because the four differ along
    axes that cut across each other -- where the binary lives, whether there is
    a control socket, what "ready" looks like -- and a chain would grow a branch
    per axis per app.
    """

    # Relative to the build directory, first match wins. The editor is a macOS
    # bundle in this tree and a plain executable elsewhere, so order matters.
    binaries: tuple[str, ...]

    # Embeds libs/agent_control: takes --mcp=<socket> and answers JSON-RPC.
    # False means supervision only -- see the module docstring.
    controllable: bool

    # The substring in the child's output that means it is up and serving.
    #
    # Controllable apps print AGENT_READY on stdout once the socket is
    # listening, which is a handshake written for this. A node has no such
    # handshake, so the marker is a log line it already emits -- chosen as the
    # LAST thing logged before it starts answering, so a match means "serving"
    # rather than "started opening files". Waiting for the process merely to
    # stay alive would not do: map_server spends seconds opening a 512 MB
    # archive, and a request sent into that window is answered as noSuchTileset.
    ready_marker: str


APPS: dict[str, AppSpec] = {
    "dashboard": AppSpec(
        binaries=("dashboard/dashboard",),
        controllable=True,
        ready_marker="AGENT_READY",
    ),
    "editor": AppSpec(
        binaries=("dashboard/editor.app/Contents/MacOS/editor", "dashboard/editor"),
        controllable=True,
        ready_marker="AGENT_READY",
    ),
    "scope": AppSpec(
        binaries=("scope/scope",),
        controllable=True,
        ready_marker="AGENT_READY",
    ),
    "map_server": AppSpec(
        binaries=("nodes/map_server/map_server",),
        controllable=False,
        # services.cpp logs this once every service is registered.
        ready_marker="[node] tiles on ",
    ),
    "bd992_mock": AppSpec(
        binaries=("nodes/bd992_mock/bd992_mock",),
        controllable=False,
        # Logged after the route or track has been fetched from map_server and
        # the publishers are open -- so a match means samples are on the bus,
        # not that it is still asking for a route. It needs extra_args to say
        # where to drive, e.g. ["--track", "Willow Springs"]; without them it
        # exits with a usage error rather than hanging, which app_launch
        # reports as a startup failure.
        ready_marker="[mock] driving at ",
    ),
}


def spec_for(app: AppName) -> AppSpec:
    try:
        return APPS[app]
    except KeyError:
        raise LaunchError(f"Unknown app '{app}'. Known: {', '.join(sorted(APPS))}") from None

# How long to wait for a process to report readiness before giving up on a
# launch. Generous because a debug build loading a large config on a cold page
# cache is genuinely slow, and a node opening a 512 MB archive more so.
READY_TIMEOUT_S = 30.0

# Lines of the child's OUTPUT -- both streams -- kept in memory. For an app this
# is crash reporting: its in-process log ring dies with the process, so this is
# what survives a segfault. For a node it is the only log there is, and what
# app_logs reads.
OUTPUT_RING = 2000


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
    controllable: bool = True
    # BOTH streams, interleaved in arrival order. spdlog writes to stdout and
    # Qt to stderr, and splitting them would put a node's whole log on one side
    # and an app's crash on the other.
    output: collections.deque[str] = field(
        default_factory=lambda: collections.deque(maxlen=OUTPUT_RING)
    )

    def alive(self) -> bool:
        return self.process.poll() is None

    def output_tail(self, lines: int = 40) -> str:
        return "".join(list(self.output)[-lines:])


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
    candidates = [build / rel for rel in spec_for(app).binaries]

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

            spec = spec_for(app)
            binary = binary_for(app)

            # A node gets no socket. Handing it one and letting the connect fail
            # later would make "this app cannot be clicked" surface as a
            # connection error at the first ui_ call, which reads as a crash.
            socket_path = os.path.join(self._tmpdir, f"{app}.sock") if spec.controllable else ""
            if socket_path and os.path.exists(socket_path):
                os.unlink(socket_path)

            argv = [str(binary)]
            if socket_path:
                argv.append(f"--mcp={socket_path}")
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
                controllable=spec.controllable,
            )

            ready = threading.Event()

            def pump(stream) -> None:
                for line in stream:
                    instance.output.append(line)
                    if spec.ready_marker in line:
                        ready.set()

            # Both streams keep their lines AND both are watched for the marker:
            # which stream a given app announces itself on is its business, and
            # spdlog and Qt do not agree about it.
            stderr_thread = threading.Thread(target=pump, args=(process.stderr,), daemon=True)
            stderr_thread.start()
            threading.Thread(target=pump, args=(process.stdout,), daemon=True).start()

            def drained_tail() -> str:
                # The pump thread ends at EOF, which arrives once the process is
                # gone. Join it before reading so the tail is complete: an app
                # that dies during startup writes its reason and exits in the
                # same breath, so without this there is a race between the pump
                # and the report -- and an error report that loses the error is
                # worse than no report.
                stderr_thread.join(timeout=2.0)
                return instance.output_tail()

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
                        f"--- output ---\n{drained_tail()}"
                    )
            else:
                process.kill()
                raise LaunchError(
                    f"{app} did not report readiness ({spec.ready_marker!r}) within "
                    f"{READY_TIMEOUT_S:g}s.\n"
                    f"--- output ---\n{drained_tail()}"
                )

            self._instances[app] = instance
            return instance

    # ------------------------------------------------------------------ access

    def get(self, app: AppName | None) -> Instance:
        with self._lock:
            live = {name: inst for name, inst in self._instances.items() if inst.alive()}

            if app is None:
                # Only controllable apps. "Which app did you mean" is a question
                # about the thing being inspected or clicked, and a node is
                # never the answer -- letting map_server into this set would
                # make every existing no-app call ambiguous the moment one is
                # running, which is exactly when they are most useful.
                pickable = {n: i for n, i in live.items() if i.controllable}
                if not pickable:
                    raise LaunchError("No application is running. Call app_launch first.")
                if len(pickable) > 1:
                    names = ", ".join(sorted(pickable))
                    raise LaunchError(
                        f"Both {names} are running; pass app= to say which one you mean."
                    )
                return next(iter(pickable.values()))

            instance = self._instances.get(app)
            if instance is None:
                raise LaunchError(f"No {app} is running. Call app_launch(app='{app}') first.")
            if not instance.alive():
                raise LaunchError(
                    f"The {app} process exited with code {instance.process.returncode}.\n"
                    f"--- output ---\n{instance.output_tail()}"
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
                        "controllable": inst.controllable,
                        "socket_path": inst.socket_path or None,
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
