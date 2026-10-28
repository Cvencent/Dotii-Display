from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

BRIDGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE))

from bluetooth_bridge import BluetoothBridge  # noqa: E402
from codex_bridge import (  # noqa: E402
    SCHEMA_VERSION,
    BridgeServer,
    DisplayConfigStore,
    FirmwareFlasher,
    McpConfigStore,
    ModuleConfigStore,
    RuntimeStatus,
    StateStore,
    XiaozhiConfigStore,
)
from qqmusic import QQMusicAccount, QQMusicNotLoggedIn  # noqa: E402

TOKEN = "management-api-test-token"


class ManagementApiTests(unittest.TestCase):
    """Drive the real management server over loopback HTTP."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="dotii-api-")
        cls.root = Path(cls.temporary.name)
        state_path = cls.root / "state.json"
        state_path.write_text(
            json.dumps(
                {
                    "schema_version": SCHEMA_VERSION,
                    "generated_at_epoch": 1,
                    "preview_data": True,
                    "source": "preview",
                }
            ),
            encoding="utf-8",
        )
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

    def request(
        self,
        method: str,
        path: str,
        *,
        payload: object | None = None,
        body: bytes | None = None,
        content_type: str = "application/json",
        token: str | None = None,
    ) -> tuple[int, bytes, str]:
        data = body
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", content_type)
        if token is not None:
            request.add_header("X-Bridge-Token", token)
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, response.read(), response.headers.get_content_type()
        except urllib.error.HTTPError as error:
            return error.code, error.read(), error.headers.get_content_type()

    def json_body(self, method: str, path: str, **kwargs: object) -> tuple[int, dict]:
        status, raw, _ = self.request(method, path, **kwargs)
        return status, json.loads(raw.decode("utf-8"))

    def test_health_and_overview_report_the_running_build(self):
        status, payload = self.json_body("GET", "/health")
        self.assertEqual(status, 200)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["schema_version"], SCHEMA_VERSION)

        status, payload = self.json_body("GET", "/api/v1/admin/overview")
        self.assertEqual(status, 200)
        self.assertEqual(payload["app"]["name"], "Dotii 管理中心")
        self.assertEqual(payload["bridge"]["port"], self.server.server_port)
        self.assertTrue(payload["bridge"]["device_url"].endswith("/api/v1/snapshot"))
        self.assertEqual(
            [module["id"] for module in payload["modules"]],
            ["xiaozhi"],
        )
        self.assertIn("xiaozhi_config", payload)
        self.assertIn("display_config", payload)
        self.assertNotIn("dotii_config", payload)

    def test_snapshot_requires_the_bridge_token(self):
        status, payload = self.json_body("GET", "/api/v1/snapshot")
        self.assertEqual(status, 401)
        self.assertEqual(payload["error"], "invalid bridge token")

        status, payload = self.json_body("GET", "/api/v1/snapshot", token="wrong")
        self.assertEqual(status, 401)

        status, payload = self.json_body("GET", "/api/v1/snapshot", token=TOKEN)
        self.assertEqual(status, 200)
        self.assertEqual(payload["schema_version"], SCHEMA_VERSION)
        self.assertIn("modules", payload)
        self.assertIn("display", payload)
        self.assertIn("xiaozhi", payload)
        self.assertEqual(payload["character"], "auto")
        self.assertTrue(payload["dotii"]["animations"])
        self.assertTrue(payload["dotii"]["fixed_states"])
        self.assertNotIn("custom", payload)
        self.assertNotIn("available_states", payload["dotii"])

    def test_snapshot_upload_rejects_unknown_schema_and_bambu(self):
        status, payload = self.json_body(
            "POST", "/api/v1/snapshot", payload={"schema_version": 2}, token=TOKEN
        )
        self.assertEqual(status, 400)
        self.assertIn("schema_version", payload["error"])

        status, _ = self.json_body(
            "POST", "/api/v1/snapshot", payload={"schema_version": 1, "bambu": {}}, token=TOKEN
        )
        self.assertEqual(status, 400)

        status, payload = self.json_body(
            "POST",
            "/api/v1/snapshot",
            payload={"schema_version": 1, "generated_at_epoch": 42, "source": "bridge"},
            token=TOKEN,
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["generated_at_epoch"], 42)

    def test_admin_routes_reject_non_json_management_actions(self):
        status, payload = self.json_body(
            "POST", "/api/v1/admin/display", body=b"{}", content_type="text/plain"
        )
        self.assertEqual(status, 400)
        self.assertIn("application/json", payload["error"])

    def test_modules_toggle_only_accepts_xiaozhi(self):
        for enabled in (False, True):
            status, payload = self.json_body(
                "POST", "/api/v1/admin/modules", payload={"id": "xiaozhi", "enabled": enabled}
            )
            self.assertEqual(status, 200, payload)
            self.assertEqual(payload["modules"]["xiaozhi"], enabled)

        status, payload = self.json_body(
            "POST", "/api/v1/admin/modules", payload={"id": "dotii", "enabled": False}
        )
        self.assertEqual(status, 400)
        self.assertIn("id must be", payload["error"])

        status, payload = self.json_body(
            "POST", "/api/v1/admin/modules", payload={"id": "unknown", "enabled": True}
        )
        self.assertEqual(status, 400)
        self.assertIn("id must be", payload["error"])

        status, _ = self.json_body(
            "POST", "/api/v1/admin/modules", payload={"id": "xiaozhi", "enabled": "yes"}
        )
        self.assertEqual(status, 400)

    def test_disabling_xiaozhi_module_syncs_the_xiaozhi_config(self):
        self.json_body("POST", "/api/v1/admin/modules", payload={"id": "xiaozhi", "enabled": False})
        status, payload = self.json_body("GET", "/api/v1/admin/overview")
        self.assertEqual(status, 200)
        self.assertFalse(payload["xiaozhi_config"]["enabled"])
        self.assertFalse(payload["modules"][0]["enabled"])
        self.json_body("POST", "/api/v1/admin/modules", payload={"id": "xiaozhi", "enabled": True})

    def test_xiaozhi_config_rejects_bad_url_and_preserves_settings(self):
        status, payload = self.json_body(
            "POST", "/api/v1/admin/xiaozhi", payload={"server_url": "example.com/x"}
        )
        self.assertEqual(status, 400)
        self.assertIn("server_url", payload["error"])

        status, payload = self.json_body(
            "POST", "/api/v1/admin/xiaozhi", payload={"server_url": "ftp://example.com/x"}
        )
        self.assertEqual(status, 400)
        self.assertIn("server_url", payload["error"])

        status, payload = self.json_body(
            "POST",
            "/api/v1/admin/xiaozhi",
            payload={"server_url": "wss://api.example.com/xiaozhi/v1/", "voice": "x"},
        )
        self.assertEqual(status, 200, payload)
        self.assertEqual(
            payload["xiaozhi_config"]["server_url"], "wss://api.example.com/xiaozhi/v1/"
        )
        self.assertEqual(payload["xiaozhi_config"]["voice"], "x")
        self.assertTrue(payload["xiaozhi_config"]["enabled"])

        status, payload = self.json_body(
            "POST",
            "/api/v1/admin/xiaozhi",
            payload={"wake_words": ["你好小智", "小土豆", "开灯"]},
        )
        self.assertEqual(status, 200, payload)
        self.assertEqual(
            payload["xiaozhi_config"]["server_url"], "wss://api.example.com/xiaozhi/v1/"
        )
        self.assertEqual(
            payload["xiaozhi_config"]["wake_word"],
            "ni hao xiao zhi;xiao tu dou;kai deng",
        )
        self.assertEqual(
            payload["xiaozhi_config"]["wake_words"], ["你好小智", "小土豆", "开灯"]
        )

    def test_display_config_round_trips_and_validates(self):
        status, payload = self.json_body(
            "POST", "/api/v1/admin/display", payload={"screen_off_timeout_seconds": 30}
        )
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload["display_config"]["screen_off_timeout_seconds"], 30)

        status, payload = self.json_body(
            "POST", "/api/v1/admin/display", payload={"screen_off_timeout_seconds": 7}
        )
        self.assertEqual(status, 400)
        self.assertIn("screen_off_timeout_seconds", payload["error"])

        status, payload = self.json_body(
            "POST", "/api/v1/admin/display", payload={"screen_off_page": "custom"}
        )
        self.assertEqual(status, 200, payload)
        self.assertEqual("none", payload["display_config"]["screen_off_page"])

        status, payload = self.json_body(
            "POST", "/api/v1/admin/display", payload={"screen_off_page": "unknown"}
        )
        self.assertEqual(status, 400)
        self.assertIn("screen_off_page", payload["error"])

    def test_removed_page_endpoints_are_gone(self):
        for method, path in (
            ("GET", "/api/v1/admin/custom/source"),
            ("GET", "/api/v1/custom-screen.rgb565"),
            ("POST", "/api/v1/admin/custom"),
            ("POST", "/api/v1/admin/custom/source"),
            ("POST", "/api/v1/admin/custom/render"),
            ("POST", "/api/v1/admin/dotii"),
        ):
            status, payload = self.json_body(method, path, payload={})
            self.assertEqual(status, 404, f"{method} {path}")
            self.assertEqual(payload["error"], "not found")

    def test_static_assets_are_served_and_directory_traversal_is_blocked(self):
        status, raw, content_type = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertTrue(content_type.startswith("text/html"))
        self.assertIn(b"<", raw)

        for path in ("/web/app.js", "/../bridge/codex_bridge.py"):
            status, _ = self.json_body("GET", path)
            self.assertEqual(status, 404, path)

    def test_settings_endpoint_validates_the_flag_without_touching_startup(self):
        status, payload = self.json_body(
            "POST", "/api/v1/admin/settings", payload={"auto_start": "yes"}
        )
        self.assertEqual(status, 400)
        self.assertIn("auto_start", payload["error"])

    def test_bluetooth_and_firmware_endpoints_report_state(self):
        status, payload = self.json_body("POST", "/api/v1/admin/firmware/refresh", payload={})
        self.assertEqual(status, 200, payload)
        self.assertIn("firmware", payload)

        status, payload = self.json_body("GET", "/api/v1/admin/overview")
        self.assertEqual(status, 200)
        self.assertIn("connection_state", payload["bluetooth"])
        self.assertIn("devices", payload["bluetooth"])
        self.assertIn("package", payload["firmware"])
        self.assertIn("ports", payload["firmware"])

        status, payload = self.json_body("POST", "/api/v1/admin/firmware/flash", payload={})
        self.assertEqual(status, 400)
        self.assertIn("确认", payload["error"])

    def test_device_music_search_reports_an_expired_session_as_unauthorized(self):
        """A dead QQ session must answer 401, not a generic 502.

        The device shows the error text from this response, so the message has
        to say the login needs refreshing rather than blaming the network.
        """
        def expired(*_args, **_kwargs):
            raise QQMusicNotLoggedIn("QQ Music login expired; sign in again.")

        original = self.server.qqmusic.search
        self.server.qqmusic.search = expired
        try:
            status, payload = self.json_body(
                "POST", "/api/v1/device/music/search",
                payload={"q": "苦瓜", "limit": 3}, token=TOKEN,
            )
        finally:
            self.server.qqmusic.search = original

        self.assertEqual(status, 401, payload)
        self.assertIn("login", payload["error"].lower())

    def test_device_music_search_success_returns_results(self):
        def found(*_args, **_kwargs):
            return [{"mid": "000UPdbN1GKIda", "title": "苦瓜",
                     "artist": "陈奕迅", "album": "", "playable": True}]

        original = self.server.qqmusic.search
        self.server.qqmusic.search = found
        try:
            status, payload = self.json_body(
                "POST", "/api/v1/device/music/search",
                payload={"q": "苦瓜", "limit": 3}, token=TOKEN,
            )
        finally:
            self.server.qqmusic.search = original

        self.assertEqual(status, 200, payload)
        self.assertEqual(payload["results"][0]["mid"], "000UPdbN1GKIda")


    def test_unknown_endpoints_return_not_found(self):
        status, payload = self.json_body("POST", "/api/v1/admin/does-not-exist", payload={})
        self.assertEqual(status, 404)
        self.assertEqual(payload["error"], "not found")


if __name__ == "__main__":
    unittest.main()
