"""JSON-RPC 2.0 client for the agent control socket embedded in the apps."""

from __future__ import annotations

import json
import socket
import threading
from typing import Any


class AgentError(RuntimeError):
    """A structured error returned by the application."""

    def __init__(self, payload: dict[str, Any]) -> None:
        data = payload.get("data") or {}
        self.reason: str = data.get("reason", "UNKNOWN")
        self.data = data
        super().__init__(payload.get("message", "unknown error"))


class AgentClient:
    """One long-lived connection to an application's control socket.

    Persistent rather than connect-per-call on purpose: the C++ side spawns a
    thread per connection, so a per-call client would churn a thread for every
    tool invocation. Reconnects transparently if the link drops.
    """

    def __init__(self, socket_path: str, timeout: float = 30.0) -> None:
        self.socket_path = socket_path
        self.timeout = timeout
        self._sock: socket.socket | None = None
        self._buf = b""
        self._next_id = 1
        self._lock = threading.Lock()

    def _connect(self) -> socket.socket:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(self.timeout)
        sock.connect(self.socket_path)
        return sock

    def close(self) -> None:
        with self._lock:
            if self._sock is not None:
                try:
                    self._sock.close()
                finally:
                    self._sock = None
                    self._buf = b""

    def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        with self._lock:
            try:
                return self._call_locked(method, params)
            except (BrokenPipeError, ConnectionResetError, OSError):
                # One transparent retry on a dropped link. A second failure is
                # real and propagates -- usually it means the app died, which the
                # supervisor reports with the process's stderr attached.
                if self._sock is not None:
                    try:
                        self._sock.close()
                    except OSError:
                        pass
                self._sock = None
                self._buf = b""
                return self._call_locked(method, params)

    def _call_locked(self, method: str, params: dict[str, Any] | None) -> Any:
        if self._sock is None:
            self._sock = self._connect()

        request_id = self._next_id
        self._next_id += 1
        request = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
            "params": {k: v for k, v in (params or {}).items() if v is not None},
        }
        self._sock.sendall((json.dumps(request) + "\n").encode())

        while b"\n" not in self._buf:
            chunk = self._sock.recv(1 << 20)
            if not chunk:
                raise ConnectionResetError("control socket closed while awaiting a reply")
            self._buf += chunk

        line, _, self._buf = self._buf.partition(b"\n")
        response = json.loads(line)

        if "error" in response:
            raise AgentError(response["error"])
        return response.get("result")
