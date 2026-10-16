from __future__ import annotations

import json
import sys
import zlib
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
    CUSTOM_FRAME_SIZE,
    SCHEMA_VERSION,
    BridgeServer,
    CustomAssetStore,
    CustomConfigStore,
    DisplayConfigStore,
    DotiiConfigStore,
    FirmwareFlasher,
    ModuleConfigStore,
    RuntimeStatus,
    StateStore,
    XiaozhiConfigStore,
)

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
            CustomConfigStore(cls.root / "custom.json"),
            ModuleConfigStore(cls.root / "modules.json"),
            DotiiConfigStore(cls.root / "dotii.json"),
            DisplayConfigStore(cls.root / "display.json"),
            CustomAssetStore(cls.root),
            XiaozhiConfigStore(cls.root / "xiaozhi.json"),
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
            ["xiaozhi", "custom", "dotii"],
        )
        self.assertIn("xiaozhi_config", payload)
        self.assertIn("display_config", payload)
        self.assertIn("dotii_config", payload)

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
        self.assertIn("custom", payload)
        self.assertIn("display", payload)
        self.assertIn("xiaozhi", payload)

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

    def test_modules_toggle_and_keep_one_page_enabled(self):
        for module_id, enabled in (
            ("xiaozhi", False),
            ("xiaozhi", True),
            ("dotii", False),
            ("dotii", True),
        ):
            status, payload = self.json_body(
                "POST", "/api/v1/admin/modules", payload={"id": module_id, "enabled": enabled}
            )
            self.assertEqual(status, 200, payload)
            self.assertEqual(payload["modules"][module_id], enabled)

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
            "POST", "/api/v1/admin/xiaozhi", payload={"wake_word": "你好小智"}
        )
        self.assertEqual(status, 200, payload)
        self.assertEqual(
            payload["xiaozhi_config"]["server_url"], "wss://api.example.com/xiaozhi/v1/"
        )

    def test_dotii_and_display_configs_round_trip_and_validate(self):
        status, payload = self.json_body(
            "POST", "/api/v1/admin/dotii", payload={"character": "dotii"}
        )
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload["dotii_config"]["character"], "dotii")
        self.assertTrue(payload["dotii_config"]["animations"])
        first_revision = payload["dotii_config"]["revision"]
        status, payload = self.json_body(
            "POST", "/api/v1/admin/dotii", payload={"character": "dotii"}
        )
        self.assertGreater(payload["dotii_config"]["revision"], first_revision)

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

    def test_custom_page_upload_render_commit_and_readback(self):
        status, payload = self.json_body("GET", "/api/v1/admin/custom/source")
        self.assertEqual(status, 404)
        self.assertIn("尚未上传", payload["error"])

        png = bytes.fromhex(
            "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
            "0000000a49444154789c63000100000500010d0a2db40000000049454e44ae426082"
        )
        status, payload = self.json_body(
            "POST", "/api/v1/admin/custom/source", body=png, content_type="image/png"
        )
        self.assertEqual(status, 200, payload)
        self.assertTrue(payload["source_available"])
        self.assertEqual(payload["source_mime"], "image/png")
        self.assertEqual(payload["source_revision"], zlib.crc32(png) & 0xFFFFFFFF)

        status, raw, content_type = self.request("GET", "/api/v1/admin/custom/source")
        self.assertEqual(status, 200)
        self.assertEqual(raw, png)
        self.assertEqual(content_type, "image/png")

        status, payload = self.json_body(
            "POST",
            "/api/v1/admin/custom/render",
            body=b"\x00" * 8,
            content_type="application/octet-stream",
        )
        self.assertEqual(status, 400)
        self.assertIn("invalid request size", payload["error"])

        status, payload = self.json_body(
            "POST",
            "/api/v1/admin/custom/render",
            body=bytes(CUSTOM_FRAME_SIZE),
            content_type="application/octet-stream",
        )
        self.assertEqual(status, 200, payload)
        render_token = payload["render_token"]

        status, payload = self.json_body(
            "POST",
            "/api/v1/admin/custom",
            payload={
                "enabled": True,
                "accent": "#102030",
                "title": "测试页面",
                "render_token": render_token,
            },
        )
        self.assertEqual(status, 200, payload)
        self.assertTrue(payload["custom"]["enabled"])
        self.assertEqual(payload["custom"]["accent"], "#102030")
        self.assertEqual(payload["custom"]["title"], "测试页面")
        self.assertTrue(payload["custom"]["image_available"])
        self.assertEqual(payload["custom"]["image_size"], CUSTOM_FRAME_SIZE)
        self.assertEqual(payload["custom"]["image_revision"], zlib.crc32(bytes(CUSTOM_FRAME_SIZE)) & 0xFFFFFFFF)

        status, raw, content_type = self.request(
            "GET", "/api/v1/custom-screen.rgb565", token=TOKEN
        )
        self.assertEqual(status, 200)
        self.assertEqual(len(raw), CUSTOM_FRAME_SIZE)
        self.assertEqual(content_type, "application/octet-stream")

        status, payload = self.json_body(
            "POST", "/api/v1/admin/custom", payload={"accent": "#zzzzzz"}
        )
        self.assertEqual(status, 400)
        self.assertIn("accent", payload["error"])

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

    def test_disabling_every_page_is_rejected(self):
        self.json_body("POST", "/api/v1/admin/custom", payload={"enabled": False})
        status, payload = self.json_body(
            "POST", "/api/v1/admin/modules", payload={"id": "dotii", "enabled": False}
        )
        self.assertEqual(status, 200, payload)
        status, payload = self.json_body(
            "POST", "/api/v1/admin/modules", payload={"id": "xiaozhi", "enabled": False}
        )
        self.assertEqual(status, 400)
        self.assertIn("至少保留一个启用页面", payload["error"])
        self.json_body("POST", "/api/v1/admin/custom", payload={"enabled": True})
        self.json_body("POST", "/api/v1/admin/modules", payload={"id": "dotii", "enabled": True})

    def test_unknown_endpoints_return_not_found(self):
        status, payload = self.json_body("POST", "/api/v1/admin/does-not-exist", payload={})
        self.assertEqual(status, 404)
        self.assertEqual(payload["error"], "not found")


if __name__ == "__main__":
    unittest.main()