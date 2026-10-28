"""Minimal MCP client used by the Dotii 管理中心.

The device forwards its xiaozhi MCP tool calls to the management center, which
relays them to whatever external MCP servers the user configured. This module
implements just enough of the Model Context Protocol for that relay:

- ``initialize`` handshake, ``tools/list`` and ``tools/call``
- transport ``http``: MCP Streamable HTTP (POST JSON-RPC, JSON or SSE reply)
- transport ``stdio``: newline-delimited JSON-RPC over a local child process

Only the standard library is used so the frozen (PyInstaller) build does not
grow new dependencies. Sessions are cached per server id; ``drop_sessions``
invalidates them after a config change.
"""

from __future__ import annotations

import json
import os
import queue
import subprocess
import threading
import time
import urllib.error
import urllib.request

MCP_PROTOCOL_VERSION = "2025-06-18"

PROBE_TIMEOUT_S = 12.0
CALL_TIMEOUT_S = 60.0
STDIO_IDLE_TIMEOUT_S = 300.0
STDIO_LINE_MAX = 1024 * 1024


class McpError(Exception):
    """Raised when an MCP server cannot be reached or answers with an error."""


def _rpc(method: str, params: dict | None = None) -> dict:
    request = {"jsonrpc": "2.0", "id": 1, "method": method}
    if params is not None:
        request["params"] = params
    return request


def _extract_text(result: dict) -> str:
    """Flatten an MCP tools/call result into the text the device speaks."""
    texts: list[str] = []
    content = result.get("content")
    if isinstance(content, list):
        for item in content:
            if not isinstance(item, dict):
                continue
            kind = item.get("type")
            if kind == "text":
                texts.append(str(item.get("text", "")))
            else:
                texts.append(f"[{kind or 'unknown'} content]")
    if not texts and result.get("structuredContent") is not None:
        texts.append(json.dumps(result["structuredContent"], ensure_ascii=False))
    return "\n".join(texts).strip()


# ---------------------------------------------------------------------------
# Streamable HTTP transport
# ---------------------------------------------------------------------------


class _HttpSession:
    """One initialized MCP session over Streamable HTTP."""

    def __init__(self) -> None:
        self.session_id: str | None = None
        self.server_info: dict = {}
        self.last_used = time.monotonic()
        self.lock = threading.Lock()

    def _post(self, server: dict, payload: dict, timeout: float):
        headers = {"Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream",
                   "MCP-Protocol-Version": MCP_PROTOCOL_VERSION}
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        for key, value in (server.get("headers") or {}).items():
            headers[str(key)] = str(value)
        request = urllib.request.Request(
            server["url"],
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        return urllib.request.urlopen(request, timeout=timeout)

    @staticmethod
    def _read_sse_live(response, wanted_id, deadline: float) -> dict:
        """Consume a live SSE body until the JSON-RPC reply for wanted_id shows up.

        Streamable HTTP servers keep the event stream open, so the reply must be
        picked off the wire line by line instead of reading until EOF.
        """
        data_lines: list[str] = []
        while time.monotonic() < deadline:
            try:
                raw = response.readline()
            except (TimeoutError, OSError) as error:
                raise McpError("MCP 服务器响应超时（等待 SSE 应答）") from error
            if not raw:
                break
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if line == "":
                for blob in data_lines:
                    try:
                        message = json.loads(blob)
                    except json.JSONDecodeError:
                        continue
                    if isinstance(message, dict) and message.get("id") == wanted_id:
                        return message
                data_lines = []
                continue
            if line.startswith("data:"):
                data_lines.append(line[5:].lstrip())
        raise McpError("MCP 服务器响应超时（SSE 流中没有收到应答）")

    def request(self, server: dict, method: str, params: dict | None,
                timeout: float) -> dict:
        payload = _rpc(method, params)
        wanted_id = payload["id"]
        with self.lock:
            self.last_used = time.monotonic()
            deadline = time.monotonic() + timeout
            try:
                response = self._post(server, payload, timeout)
            except urllib.error.HTTPError as error:
                # A 404/405 usually means the server dropped our session.
                if self.session_id and error.code in (400, 404, 405):
                    self.session_id = None
                    raise McpError(f"MCP 服务器拒绝了请求（HTTP {error.code}），会话已重置") from error
                raise McpError(f"MCP 服务器返回 HTTP {error.code}") from error
            except (urllib.error.URLError, OSError) as error:
                raise McpError(f"无法连接 MCP 服务器：{error}") from error
            try:
                new_session = response.headers.get("Mcp-Session-Id")
                if new_session:
                    self.session_id = new_session
                content_type = response.headers.get("Content-Type", "")
                if "text/event-stream" in content_type:
                    message = self._read_sse_live(response, wanted_id, deadline)
                    return message
                body = response.read(4 * 1024 * 1024)
            finally:
                response.close()
            try:
                message = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise McpError("MCP 服务器返回了无法解析的响应") from error
            if not isinstance(message, dict) or message.get("id") != wanted_id:
                raise McpError("MCP 服务器应答与请求不匹配")
            if "error" in message:
                detail = message["error"]
                text = detail.get("message") if isinstance(detail, dict) else str(detail)
                raise McpError(f"MCP 服务器错误：{text}")
            return message.get("result") or {}

    def initialize(self, server: dict, timeout: float) -> None:
        result = self.request(server, "initialize", {
            "protocolVersion": MCP_PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "dotii-management-center", "version": "1.0"},
        }, timeout)
        self.server_info = result.get("serverInfo") or {}
        # Streamable HTTP expects the initialized notification right after.
        self.notify(server, "notifications/initialized")

    def notify(self, server: dict, method: str) -> None:
        payload = {"jsonrpc": "2.0", "method": method}
        headers = {"Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream",
                   "MCP-Protocol-Version": MCP_PROTOCOL_VERSION}
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        for key, value in (server.get("headers") or {}).items():
            headers[str(key)] = str(value)
        request = urllib.request.Request(
            server["url"],
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=10.0) as response:
                response.read()
        except (urllib.error.URLError, OSError):
            pass  # notifications are best-effort


# ---------------------------------------------------------------------------
# stdio transport
# ---------------------------------------------------------------------------


class _StdioSession:
    """One local MCP server process speaking newline-delimited JSON-RPC."""

    def __init__(self, server: dict) -> None:
        self.lock = threading.Lock()
        self.last_used = time.monotonic()
        self.server_info: dict = {}
        self._replies: queue.Queue = queue.Queue()
        self._closed = threading.Event()
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
        env = dict(os.environ)
        env.update({str(k): str(v) for k, v in (server.get("env") or {}).items()})
        try:
            self.process = subprocess.Popen(
                [server["command"], *[str(a) for a in (server.get("args") or [])]],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=env,
                creationflags=creationflags,
            )
        except OSError as error:
            raise McpError(f"无法启动 MCP 程序：{error}") from error
        self._reader = threading.Thread(target=self._read_loop, daemon=True,
                                        name=f"mcp-stdio-{server.get('id')}")
        self._reader.start()
        self.request({"initialize": {
            "protocolVersion": MCP_PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "dotii-management-center", "version": "1.0"},
        }}, PROBE_TIMEOUT_S)
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _read_loop(self) -> None:
        stream = self.process.stdout
        while not self._closed.is_set():
            raw = stream.readline()
            if not raw:
                break
            if len(raw) > STDIO_LINE_MAX:
                continue
            try:
                message = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if isinstance(message, dict) and "id" in message:
                self._replies.put(message)
        self._replies.put(None)

    def _send(self, payload: dict) -> None:
        try:
            self.process.stdin.write(json.dumps(payload, ensure_ascii=False).encode("utf-8") + b"\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise McpError("MCP 程序已退出") from error

    def request(self, spec: dict, timeout: float) -> dict:
        """spec maps a single {"method": params}; the id is assigned here."""
        (method, params), = spec.items()
        request_id = 1
        payload = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            payload["params"] = params
        with self.lock:
            self.last_used = time.monotonic()
            self._send(payload)
            deadline = time.monotonic() + timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise McpError(f"MCP 程序响应超时（{method}）")
                try:
                    message = self._replies.get(timeout=remaining)
                except queue.Empty:
                    continue
                if message is None:
                    raise McpError("MCP 程序在应答前退出")
                if message.get("id") != request_id:
                    continue  # stale reply from an earlier timed-out call
                if "error" in message:
                    detail = message["error"]
                    text = detail.get("message") if isinstance(detail, dict) else str(detail)
                    raise McpError(f"MCP 程序错误：{text}")
                return message.get("result") or {}

    def close(self) -> None:
        self._closed.set()
        try:
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.terminate()
            self.process.wait(timeout=3)
        except (subprocess.TimeoutExpired, OSError):
            try:
                self.process.kill()
            except OSError:
                pass


# ---------------------------------------------------------------------------
# Session registry
# ---------------------------------------------------------------------------

_sessions: dict[int, object] = {}
_registry_lock = threading.Lock()


def _http_session(server: dict) -> _HttpSession:
    """Return the cached session, initializing outside the registry lock so a
    slow server never serializes the other probes."""
    with _registry_lock:
        session = _sessions.get(server["id"])
        if isinstance(session, _HttpSession):
            return session
    session = _HttpSession()
    session.initialize(server, PROBE_TIMEOUT_S)
    with _registry_lock:
        existing = _sessions.get(server["id"])
        if isinstance(existing, _HttpSession):
            return existing  # a concurrent probe raced us and won
        _sessions[server["id"]] = session
        return session


def _stdio_session(server: dict) -> _StdioSession:
    with _registry_lock:
        session = _sessions.get(server["id"])
        if isinstance(session, _StdioSession) and session.process.poll() is None:
            return session
    session = _StdioSession(server)
    with _registry_lock:
        existing = _sessions.get(server["id"])
        if isinstance(existing, _StdioSession) and existing.process.poll() is None:
            session.close()
            return existing
        _sessions[server["id"]] = session
        return session


def drop_sessions() -> None:
    """Forget every cached session (called after a config change)."""
    with _registry_lock:
        for session in _sessions.values():
            if isinstance(session, _StdioSession):
                session.close()
        _sessions.clear()


def reap_idle_sessions() -> None:
    """Terminate stdio processes that have been idle for a while."""
    now = time.monotonic()
    with _registry_lock:
        for server_id, session in list(_sessions.items()):
            if isinstance(session, _StdioSession) and \
                    now - session.last_used > STDIO_IDLE_TIMEOUT_S:
                session.close()
                del _sessions[server_id]


def probe_tools(server: dict) -> list[dict]:
    """Return the server's tool list: [{name, description, inputSchema}]."""
    if server.get("transport") == "stdio":
        session = _stdio_session(server)
        result = session.request({"tools/list": {}}, PROBE_TIMEOUT_S)
    else:
        session = _http_session(server)
        result = session.request(server, "tools/list", {}, PROBE_TIMEOUT_S)
    tools = result.get("tools")
    if not isinstance(tools, list):
        return []
    cleaned = []
    for tool in tools:
        if not isinstance(tool, dict) or not isinstance(tool.get("name"), str):
            continue
        cleaned.append({
            "name": tool["name"],
            "description": tool.get("description") or tool["name"],
            "inputSchema": tool.get("inputSchema") if isinstance(tool.get("inputSchema"), dict)
                           else {"type": "object"},
        })
    return cleaned


def call_tool(server: dict, name: str, arguments: dict) -> dict:
    """Run tools/call. Returns {"text": str, "is_error": bool}."""
    if server.get("transport") == "stdio":
        session = _stdio_session(server)
        result = session.request({"tools/call": {"name": name, "arguments": arguments}},
                                 CALL_TIMEOUT_S)
    else:
        session = _http_session(server)
        result = session.request(server, "tools/call",
                                 {"name": name, "arguments": arguments}, CALL_TIMEOUT_S)
    return {"text": _extract_text(result), "is_error": bool(result.get("isError"))}
