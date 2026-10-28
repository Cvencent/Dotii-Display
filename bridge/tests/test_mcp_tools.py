from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BRIDGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE))

from mcp_client import (  # noqa: E402
    drop_sessions,
    call_tool,
    probe_tools,
)
from codex_bridge import (  # noqa: E402
    McpConfigStore,
    validate_mcp_config,
    bridge_mcp_tool_name,
)

TOKEN = "mcp-test-token"
FIXTURE = Path(__file__).parent / "stdio_mcp_fixture.py"


# ---------------------------------------------------------------------------
# A minimal Streamable-HTTP MCP server used as the remote side.
# ---------------------------------------------------------------------------


class _FakeMcpHandler(BaseHTTPRequestHandler):
    server_version = "FakeMCP/1.0"

    def _reply(self, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", 0))
        try:
            request = json.loads(self.rfile.read(length))
        except json.JSONDecodeError:
            self._reply({"jsonrpc": "2.0", "id": None,
                         "error": {"code": -32700, "message": "parse error"}})
            return
        method = request.get("method")
        if method == "initialize":
            self._reply({"jsonrpc": "2.0", "id": request["id"], "result": {
                "protocolVersion": "2025-06-18",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "fake-remote", "version": "1.0"},
            }})
        elif method == "notifications/initialized":
            self.send_response(HTTPStatus.ACCEPTED)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif method == "tools/list":
            self._reply({"jsonrpc": "2.0", "id": request["id"], "result": {"tools": [
                {"name": "get_weather", "description": "查询城市天气",
                 "inputSchema": {"type": "object",
                                 "properties": {"city": {"type": "string"}}}},
            ]}})
        elif method == "tools/call":
            city = request["params"].get("arguments", {}).get("city", "?")
            self._reply({"jsonrpc": "2.0", "id": request["id"], "result": {
                "content": [{"type": "text", "text": f"weather:{city}"}],
                "isError": False}})
        else:
            self._reply({"jsonrpc": "2.0", "id": request.get("id"),
                         "error": {"code": -32601,
                                   "message": f"no method {method}"}})

    def log_message(self, format_string: str, *args: object) -> None:
        pass


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


class McpConfigValidationTests(unittest.TestCase):
    def test_defaults_are_empty_and_enabled(self) -> None:
        config = validate_mcp_config(None)
        self.assertTrue(config["enabled"])
        self.assertEqual(config["servers"], [])
        self.assertEqual(config["revision"], 0)

    def test_http_server_round_trips(self) -> None:
        config = validate_mcp_config({
            "servers": [{"name": "天气", "transport": "http",
                         "url": "https://mcp.example.com/mcp",
                         "headers": {"Authorization": "Bearer abc"}}],
        })
        self.assertEqual(config["servers"][0]["id"], 1)
        self.assertTrue(config["servers"][0]["enabled"])
        self.assertEqual(config["servers"][0]["headers"]["Authorization"], "Bearer abc")

    def test_stdio_server_keeps_args_and_env(self) -> None:
        config = validate_mcp_config({
            "servers": [{"name": "本地", "transport": "stdio", "command": "npx",
                         "args": ["-y", "server-thing"], "env": {"KEY": "v"}}],
        })
        server = config["servers"][0]
        self.assertEqual(server["command"], "npx")
        self.assertEqual(server["args"], ["-y", "server-thing"])
        self.assertEqual(server["env"], {"KEY": "v"})

    def test_rejects_bad_transport(self) -> None:
        with self.assertRaises(ValueError):
            validate_mcp_config({"servers": [{"name": "x", "transport": "ws",
                                              "url": "https://a/b"}]})

    def test_rejects_non_http_url(self) -> None:
        with self.assertRaises(ValueError):
            validate_mcp_config({"servers": [{"name": "x", "transport": "http",
                                              "url": "ftp://a/b"}]})

    def test_rejects_stdio_without_command(self) -> None:
        with self.assertRaises(ValueError):
            validate_mcp_config({"servers": [{"name": "x", "transport": "stdio"}]})

    def test_rejects_duplicate_ids_and_names(self) -> None:
        base = {"transport": "http", "url": "https://a/b"}
        with self.assertRaises(ValueError):
            validate_mcp_config({"servers": [
                {**base, "id": 3, "name": "a"}, {**base, "id": 3, "name": "b"}]})
        with self.assertRaises(ValueError):
            validate_mcp_config({"servers": [{**base, "name": "a"}, {**base, "name": "a"}]})

    def test_rejects_too_many_servers(self) -> None:
        servers = [{"name": f"s{i}", "transport": "http", "url": "https://a/b"}
                   for i in range(9)]
        with self.assertRaises(ValueError):
            validate_mcp_config({"servers": servers})

    def test_tool_name_is_sanitized_and_prefixed(self) -> None:
        self.assertEqual(bridge_mcp_tool_name(2, "get_weather"), "mcp2_get_weather")
        self.assertEqual(bridge_mcp_tool_name(1, "fs.read/file"), "mcp1_fs_read_file")
        self.assertTrue(len(bridge_mcp_tool_name(1, "x" * 200)) <= 64)


# ---------------------------------------------------------------------------
# MCP client transports against the fakes
# ---------------------------------------------------------------------------


class McpTransportTests(unittest.TestCase):
    def setUp(self) -> None:
        drop_sessions()
        self.http_server = ThreadingHTTPServer(("127.0.0.1", 0), _FakeMcpHandler)
        self.http_thread = threading.Thread(target=self.http_server.serve_forever,
                                            daemon=True)
        self.http_thread.start()
        self.http_url = f"http://127.0.0.1:{self.http_server.server_port}/mcp"

    def tearDown(self) -> None:
        drop_sessions()
        self.http_server.shutdown()
        self.http_server.server_close()

    def test_http_probe_and_call(self) -> None:
        server = {"id": 1, "name": "远程", "transport": "http", "url": self.http_url}
        tools = probe_tools(server)
        self.assertEqual([t["name"] for t in tools], ["get_weather"])
        result = call_tool(server, "get_weather", {"city": "北京"})
        self.assertFalse(result["is_error"])
        self.assertEqual(result["text"], "weather:北京")

    def test_stdio_probe_and_call(self) -> None:
        server = {"id": 2, "name": "本地", "transport": "stdio",
                  "command": sys.executable, "args": [str(FIXTURE)]}
        tools = probe_tools(server)
        self.assertEqual([t["name"] for t in tools], ["echo"])
        result = call_tool(server, "echo", {"text": "你好"})
        self.assertEqual(result["text"], 'echo:{"text": "你好"}')

    def test_http_unreachable_reports_error(self) -> None:
        server = {"id": 3, "name": "坏", "transport": "http",
                  "url": "http://127.0.0.1:1/mcp"}
        with self.assertRaises(Exception):
            probe_tools(server)


# ---------------------------------------------------------------------------
# Endpoints through the real management server
# ---------------------------------------------------------------------------


class McpEndpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        from bluetooth_bridge import BluetoothBridge
        from codex_bridge import (
            BridgeServer,
            DisplayConfigStore,
            FirmwareFlasher,
            ModuleConfigStore,
            RuntimeStatus,
            StateStore,
            XiaozhiConfigStore,
        )
        from qqmusic import QQMusicAccount

        cls.temporary = tempfile.TemporaryDirectory(prefix="dotii-mcp-")
        cls.root = Path(cls.temporary.name)
        state_path = cls.root / "state.json"
        state_path.write_text(json.dumps({
            "schema_version": 1, "generated_at_epoch": 1,
            "preview_data": True, "source": "preview",
        }), encoding="utf-8")
        cls.server = BridgeServer(
            ("127.0.0.1", 0),
            StateStore(state_path),
            TOKEN,
            RuntimeStatus(),
            ModuleConfigStore(cls.root / "modules.json"),
            DisplayConfigStore(cls.root / "display.json"),
            XiaozhiConfigStore(cls.root / "xiaozhi.json"),
            McpConfigStore(cls.root / "mcp.json"),
            QQMusicAccount(cls.root),
            BluetoothBridge(cls.root),
            FirmwareFlasher(BRIDGE.parent, cls.root),
            cls.root,
        )
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=5)
        cls.temporary.cleanup()

    def request(self, method: str, path: str, payload: dict | None = None,
                token: str | None = TOKEN) -> tuple[int, dict]:
        data = json.dumps(payload).encode() if payload is not None else None
        request = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        if token is not None:
            request.add_header("X-Bridge-Token", token)
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def setUp(self) -> None:
        drop_sessions()
        self.http_server = ThreadingHTTPServer(("127.0.0.1", 0), _FakeMcpHandler)
        threading.Thread(target=self.http_server.serve_forever, daemon=True).start()
        self.http_url = f"http://127.0.0.1:{self.http_server.server_port}/mcp"

    def tearDown(self) -> None:
        drop_sessions()
        self.http_server.shutdown()
        self.http_server.server_close()
        self.server.mcp.write({"enabled": True, "revision": 0, "servers": []})

    def test_device_tools_requires_token(self) -> None:
        status, _ = self.request("GET", "/api/v1/device/mcp/tools", token=None)
        self.assertEqual(status, 401)

    def test_empty_config_returns_empty_tool_list(self) -> None:
        status, body = self.request("GET", "/api/v1/device/mcp/tools")
        self.assertEqual(status, 200)
        self.assertEqual(body["tools"], [])
        self.assertEqual(body["revision"], 0)

    def test_config_round_trip_and_snapshot_revision(self) -> None:
        payload = {"enabled": True, "servers": [
            {"name": "天气", "transport": "http", "url": self.http_url}]}
        status, body = self.request("POST", "/api/v1/admin/mcp", payload)
        self.assertEqual(status, 200)
        self.assertEqual(body["mcp_config"]["servers"][0]["id"], 1)
        self.assertEqual(body["mcp_config"]["revision"], 1)
        _status, snapshot = self.request("GET", "/api/v1/snapshot")
        self.assertEqual(snapshot["mcp"], {"enabled": True, "revision": 1,
                                           "server_count": 1})

    def test_device_sees_proxied_tool_and_calls_it(self) -> None:
        self.request("POST", "/api/v1/admin/mcp", {"servers": [
            {"name": "天气", "transport": "http", "url": self.http_url}]})
        status, body = self.request("GET", "/api/v1/device/mcp/tools")
        self.assertEqual(status, 200)
        names = [tool["name"] for tool in body["tools"]]
        self.assertEqual(names, ["mcp1_get_weather"])
        self.assertIn("天气", body["tools"][0]["description"])
        self.assertEqual(body["tools"][0]["inputSchema"]["type"], "object")

        status, reply = self.request("POST", "/api/v1/device/mcp/call", {
            "name": "mcp1_get_weather", "arguments": {"city": "上海"}})
        self.assertEqual(status, 200)
        self.assertTrue(reply["ok"])
        self.assertEqual(reply["text"], "weather:上海")
        self.assertFalse(reply["is_error"])

    def test_unknown_tool_call_reports_not_found(self) -> None:
        self.request("POST", "/api/v1/admin/mcp", {"servers": [
            {"name": "天气", "transport": "http", "url": self.http_url}]})
        status, reply = self.request("POST", "/api/v1/device/mcp/call", {
            "name": "mcp1_no_such_tool", "arguments": {}})
        self.assertEqual(status, 404)
        self.assertIn("未知工具", reply["error"])

    def test_disabled_master_switch_blocks_calls(self) -> None:
        self.request("POST", "/api/v1/admin/mcp", {"enabled": False, "servers": [
            {"name": "天气", "transport": "http", "url": self.http_url}]})
        status, _ = self.request("POST", "/api/v1/device/mcp/call", {
            "name": "mcp1_get_weather", "arguments": {}})
        self.assertEqual(status, 503)

    def test_admin_probe_reports_unreachable_server(self) -> None:
        self.request("POST", "/api/v1/admin/mcp", {"servers": [
            {"name": "坏服务", "transport": "http", "url": "http://127.0.0.1:1/mcp"}]})
        status, body = self.request("GET", "/api/v1/admin/mcp/tools")
        self.assertEqual(status, 200)
        self.assertEqual(len(body["servers"]), 1)
        self.assertIsNotNone(body["servers"][0]["error"])

    def test_admin_probe_single_server(self) -> None:
        self.request("POST", "/api/v1/admin/mcp", {"servers": [
            {"name": "天气", "transport": "http", "url": self.http_url}]})
        status, body = self.request("GET", "/api/v1/admin/mcp/tools?id=1")
        self.assertEqual(status, 200)
        self.assertIsNone(body["servers"][0]["error"])
        self.assertEqual(body["servers"][0]["tools"][0]["name"], "get_weather")


if __name__ == "__main__":
    unittest.main()
