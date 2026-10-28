from __future__ import annotations

import sys
import tempfile
import unittest
import json
import plistlib
import re
from pathlib import Path
from unittest import mock

BRIDGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE))

import codex_bridge  # noqa: E402
from platforms.windows import WindowsPlatformAdapter  # noqa: E402

from codex_bridge import (  # noqa: E402
    default_dotii_config,
    validate_module_config,
    validate_snapshot,
    validate_xiaozhi_config,
    watch_parent,
)



class ModuleOnDemandTests(unittest.TestCase):
    def test_management_only_packaging_can_reuse_the_formal_firmware_bundle(self):
        script = (BRIDGE.parent / "packaging" / "build_windows.ps1").read_text(encoding="utf-8")

        self.assertIn("$BuildFirmwareManifest", script)
        self.assertIn("$BundledFirmwareManifest", script)
        self.assertIn("$RefreshFirmwareBundle = $false", script)

    def test_macos_info_plist_declares_bluetooth_usage(self):
        plist_path = BRIDGE.parent / "packaging" / "macos" / "Info.plist"
        with plist_path.open("rb") as stream:
            info = plistlib.load(stream)

        self.assertEqual(info["LSMinimumSystemVersion"], "13.0")
        self.assertIn("Dotii", info["NSBluetoothAlwaysUsageDescription"])
        self.assertIn("Dotii", info["NSLocalNetworkUsageDescription"])
        self.assertEqual(info["CFBundleShortVersionString"], "1.2.2")
        self.assertEqual(info["CFBundleIconName"], "AppIcon")

    def test_macos_brand_icons_are_complete_and_used_by_native_host(self):
        root = BRIDGE.parent
        app_root = root / "macos" / "DotiiManagementCenter" / "DotiiManagementCenter"
        app_icon = app_root / "Assets.xcassets" / "AppIcon.appiconset"
        status_icon = app_root / "Assets.xcassets" / "StatusBarIcon.imageset"
        swift = (app_root / "main.swift").read_text(encoding="utf-8")

        for asset_set in (app_icon, status_icon):
            manifest = json.loads((asset_set / "Contents.json").read_text(encoding="utf-8"))
            for image in manifest["images"]:
                self.assertTrue((asset_set / image["filename"]).is_file())

        status_manifest = json.loads((status_icon / "Contents.json").read_text(encoding="utf-8"))
        self.assertEqual(status_manifest["properties"]["template-rendering-intent"], "template")
        self.assertIn('NSImage(named: "StatusBarIcon")', swift)
        self.assertIn("isTemplate = true", swift)

    def test_macos_native_host_and_packaging_sources_are_present(self):
        root = BRIDGE.parent
        swift = (root / "macos" / "DotiiManagementCenter" / "DotiiManagementCenter" / "main.swift").read_text(encoding="utf-8")
        packaging = (root / "packaging" / "macos" / "build_macos.sh").read_text(encoding="utf-8")

        self.assertIn("NSStatusBar.system.statusItem", swift)
        self.assertIn("SMAppService.mainApp", swift)
        self.assertIn('"--parent-pid"', swift)
        self.assertIn('"--host", "0.0.0.0"', swift)
        self.assertNotIn('"--host", "127.0.0.1"', swift)
        self.assertIn("PYINSTALLER_CONFIG_DIR", packaging)
        self.assertIn("codesign --verify", packaging)
        self.assertIn("notarytool submit", packaging)

    def test_windows_host_serves_snapshots_on_the_lan(self):
        host = (BRIDGE / "bridge_app.py").read_text(encoding="utf-8")

        self.assertIn('"--host", "0.0.0.0"', host)
        self.assertNotIn('"--host", "127.0.0.1"', host)

    def test_parent_watchdog_stops_orphaned_backend(self):
        stop = mock.Mock()
        stop.wait.side_effect = [False]
        server = mock.Mock()

        with mock.patch.object(codex_bridge.os, "kill", side_effect=ProcessLookupError):
            watch_parent(43210, server, stop)

        server.shutdown.assert_called_once_with()

    def test_http_response_ignores_clients_that_disconnect_early(self):
        backend = (BRIDGE / "codex_bridge.py").read_text(encoding="utf-8")

        self.assertIn("BrokenPipeError, ConnectionAbortedError, ConnectionResetError", backend)

    def test_first_run_defaults_only_enable_xiaozhi(self):
        self.assertEqual(
            validate_module_config(None),
            {"xiaozhi": True},
        )

    def test_xiaozhi_config_defaults_and_requires_websocket_url(self):
        defaults = validate_xiaozhi_config(None)
        self.assertTrue(defaults["enabled"])
        self.assertEqual(defaults["server_url"], "https://api.tenclass.net/xiaozhi/ota/")
        self.assertEqual(defaults["wake_word"], "ni hao xiao zhi")
        self.assertEqual(defaults["wake_words"], ["你好小智"])
        self.assertEqual(validate_xiaozhi_config({"server_url": "https://example.com"})["server_url"],
                         "https://example.com")
        with self.assertRaises(ValueError):
            validate_xiaozhi_config({"server_url": "ftp://example.com"})

    def test_snapshot_validation_rejects_bambu_module(self):
        with self.assertRaises(ValueError):
            validate_snapshot({"schema_version": 1, "bambu": {"configured": True}})

    def test_dotii_expression_mapping_is_static_device_contract(self):
        config = default_dotii_config()

        self.assertEqual(config["character"], "auto")
        self.assertEqual(set(config["animations"]), set(codex_bridge.DOTII_EXPRESSION_IDS))
        self.assertEqual(
            config["animations"]["working"]["states"], ["codex_working"]
        )
        self.assertEqual(
            config["animations"]["complete"]["states"], ["codex_completed"]
        )
        self.assertEqual(
            [item["id"] for item in config["fixed_states"]],
            ["idle", "blink", "long_idle", "touch", "connecting"],
        )
        # The deleted management page must not leak its editing metadata again.
        self.assertNotIn("available_states", config)
        self.assertNotIn("duration_options", config)

    def test_xiaozhi_config_preserves_websocket_settings(self):
        saved = validate_xiaozhi_config({
            "enabled": True,
            "server_url": "wss://example.test/xiaozhi",
            "wake_word": "嗨小智",
            "voice": "sweet",
            "mcp_enabled": False,
        })
        self.assertEqual(saved["server_url"], "wss://example.test/xiaozhi")
        self.assertTrue(saved["enabled"])
        self.assertEqual(saved["wake_word"], "hai xiao zhi")
        self.assertEqual(saved["wake_words"], ["嗨小智"])
        self.assertEqual(saved["voice"], "sweet")
        self.assertFalse(saved["mcp_enabled"])

    def test_xiaozhi_accepts_three_chinese_wake_words_and_converts_to_pinyin(self):
        saved = validate_xiaozhi_config({
            "wake_words": ["你好小智", "小土豆", "开灯"],
        })
        self.assertEqual(saved["wake_words"], ["你好小智", "小土豆", "开灯"])
        self.assertEqual(saved["wake_word"], "ni hao xiao zhi;xiao tu dou;kai deng")

    def test_xiaozhi_rejects_more_than_three_wake_words(self):
        with self.assertRaises(ValueError):
            validate_xiaozhi_config({"wake_words": ["一", "二", "三", "四"]})

    def test_xiaozhi_wake_word_input_rejects_punctuation(self):
        with self.assertRaises(ValueError):
            validate_xiaozhi_config({"wake_words": ["你好，小智"]})

    def test_removed_pages_are_deleted_from_the_backend(self):
        backend = (BRIDGE / "codex_bridge.py").read_text(encoding="utf-8")

        for removed in (
            "CustomConfigStore",
            "CustomAssetStore",
            "DotiiConfigStore",
            "validate_custom_config",
            "validate_dotii_config",
            "/api/v1/admin/custom",
            "/api/v1/admin/dotii",
            "/api/v1/custom-screen.rgb565",
        ):
            self.assertNotIn(removed, backend)

    def test_frontend_overview_references_only_existing_status_elements(self):
        html = (BRIDGE / "web" / "index.html").read_text(encoding="utf-8")
        script = (BRIDGE / "web" / "app.js").read_text(encoding="utf-8")
        ids = set(re.findall(r'id="([^"]+)"', html))
        refs = set()
        for pattern in (
            r'byId\("([^"]+)"\)',
            r'setText\("([^"]+)"',
            r'setMarkdown\("([^"]+)"',
            r'setMarkdownMessages\("([^"]+)"',
        ):
            refs.update(re.findall(pattern, script))
        self.assertEqual(refs - ids, set())

    def test_frontend_only_keeps_the_xiaozhi_page(self):
        html = (BRIDGE / "web" / "index.html").read_text(encoding="utf-8")
        script = (BRIDGE / "web" / "app.js").read_text(encoding="utf-8")
        styles = (BRIDGE / "web" / "styles.css").read_text(encoding="utf-8")

        for removed in (
            'id="panel-custom"',
            'id="panel-dotii"',
            'id="custom-crop-dialog"',
            'id="custom-form"',
            'id="dotii-config-form"',
        ):
            self.assertNotIn(removed, html)
        for removed in (
            "customFormValue",
            "renderCustomPreview",
            "renderDotiiConfigEditor",
            "showDotiiExpression",
        ):
            self.assertNotIn(removed, script)
        for removed in (".custom-", ".dotii-", ".crop-"):
            self.assertNotIn(removed, styles)
        self.assertFalse((BRIDGE / "web" / "assets" / "expressions").exists())
        self.assertFalse((BRIDGE / "web" / "assets" / "custom.svg").exists())

    def test_frontend_has_xiaozhi_navigation_without_bambu(self):
        html = (BRIDGE / "web" / "index.html").read_text(encoding="utf-8")
        self.assertIn('data-panel="xiaozhi"', html)
        self.assertNotIn('data-panel="custom"', html)
        self.assertNotIn('data-panel="dotii"', html)
        self.assertNotIn("Bambu", html)

    def test_frontend_snapshot_does_not_expose_bambu_status(self):
        script = (BRIDGE / "web" / "app.js").read_text(encoding="utf-8")
        self.assertNotIn("renderBambu", script)
        self.assertNotIn("bambu-nav-state", script)

    def test_frontend_exposes_xiaozhi_form_without_character_navigation(self):
        html = (BRIDGE / "web" / "index.html").read_text(encoding="utf-8")
        script = (BRIDGE / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn('id="xiaozhi-form"', html)
        self.assertIn('id="xiaozhi-server-url"', html)
        self.assertIn('id="xiaozhi-wake-word-1"', html)
        self.assertIn('id="xiaozhi-wake-word-3"', html)
        self.assertIn("saveXiaozhi", script)
        self.assertIn("/api/v1/admin/xiaozhi", script)
        self.assertNotIn('data-panel="dotii"', html)
        self.assertNotIn('data-value="custom"', html)
        self.assertNotIn('data-value="dotii"', html)

    def test_frontend_exposes_xiaozhi_activation_pairing_flow(self):
        html = (BRIDGE / "web" / "index.html").read_text(encoding="utf-8")
        script = (BRIDGE / "web" / "app.js").read_text(encoding="utf-8")
        styles = (BRIDGE / "web" / "styles.css").read_text(encoding="utf-8")
        self.assertIn('id="xiaozhi-activation-code"', html)
        self.assertIn('id="xiaozhi-pairing-link"', html)
        self.assertIn("activation_required", script)
        self.assertIn(".xiaozhi-activation", styles)
        self.assertIn(".activation-code", styles)


    def test_macos_build_rejects_stale_bundled_web_assets(self):
        packaging = (BRIDGE.parent / "packaging" / "macos" / "build_macos.sh").read_text(
            encoding="utf-8"
        )

        self.assertIn('for asset in index.html app.js styles.css markdown.js', packaging)
        self.assertIn('Bundled web asset mismatch', packaging)
        self.assertIn('/bin/ln -s /Applications', packaging)
        self.assertIn('cd "$OUTPUT"', packaging)


if __name__ == "__main__":
    unittest.main()
