"""Entry point: runs the MCP server over stdio."""

from __future__ import annotations

from .server import mcp, supervisor


def main() -> None:
    try:
        mcp.run()
    finally:
        # Never leave headless apps running after the client disconnects; they
        # hold a zenoh session and would keep answering on a stale socket.
        supervisor.shutdown()


if __name__ == "__main__":
    main()
