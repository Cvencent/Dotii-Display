from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

BRIDGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE))

def _has_bleak() -> bool:
    try:
        import bleak  # noqa: F401
    except (ImportError, OSError):
        return False
    return True


BLEAK_AVAILABLE = _has_bleak()

from platforms.macos import MacOSPlatformAdapter, parse_serial_ports as parse_macos_ports  # noqa: E402
from platforms.windows import WindowsPlatformAdapter, parse_serial_ports  # noqa: E402
from bluetooth_bridge import BluetoothBridge  # noqa: E402
import runtime_paths  # noqa: E402
import bridge_app  # noqa: E402


class WindowsPlatformAdapterTests(unittest.TestCase):
    def test_windows_capabilities_remain_available(self) -> None:
        adapter = WindowsPlatformAdapter()
        self.assertTrue(adapter.startup_available)
        self.assertTrue(adapter.bluetooth_available)
        self.assertTrue(adapter.bluetooth_pair_on_connect)
        self.assertTrue(adapter.serial_flash_available)

    def test_runtime_folder_keeps_existing_windows_location(self) -> None:
        with tempfile.TemporaryDirectory(dir=BRIDGE.parent / ".codx") as temporary:
            with mock.patch.dict("os.environ", {"LOCALAPPDATA": temporary}, clear=False):
                self.assertEqual(
                    WindowsPlatformAdapter().runtime_folder(),
                    Path(temporary) / "StateDisplay",
                )

    def test_packaged_startup_keeps_management_center_entry(self) -> None:
        with tempfile.TemporaryDirectory(dir=BRIDGE.parent / ".codx") as temporary:
            executable = Path(temporary) / "DotiiManagementCenter.exe"
            command = WindowsPlatformAdapter().packaged_startup_command(executable)
            self.assertEqual(command, f'"{executable.resolve()}" --startup')
            self.assertNotIn("DotiiBridge.exe", command)

    def test_serial_payload_stays_sorted_with_dotii_first(self) -> None:
        payload = json.dumps([
            {"DeviceID": "COM8", "Name": "Other", "PNPDeviceID": "USB\\VID_1234&PID_5678"},
            {"DeviceID": "COM17", "Name": "Dotii", "PNPDeviceID": "USB\\VID_303A&PID_1001"},
        ])
        ports = parse_serial_ports(payload)
        self.assertEqual([item["port"] for item in ports], ["COM17", "COM8"])
        self.assertTrue(ports[0]["dotii"])

    def test_only_the_paired_board_is_marked_as_dotii(self) -> None:
        payload = json.dumps([
            {
                "DeviceID": "COM4",
                "Name": "USB 串行设备 (COM4)",
                "PNPDeviceID": "USB\\VID_303A&PID_1001&MI_00\\9&6B46F6C&0&0000",
                "Serial": "98:C3:77:F5:52:70",
            },
            {
                "DeviceID": "COM3",
                "Name": "USB 串行设备 (COM3)",
                "PNPDeviceID": "USB\\VID_303A&PID_1001&MI_00\\7&3261F339&0&0000",
                "Serial": "44:BD:8D:60:7F:8C",
            },
        ])
        ports = parse_serial_ports(payload, dotii_serials={"44bd8d607f8c"})
        self.assertEqual([item["port"] for item in ports], ["COM3", "COM4"])
        self.assertTrue(ports[0]["dotii"])
        self.assertFalse(ports[1]["dotii"])

    def test_unknown_boards_keep_the_vid_pid_fallback(self) -> None:
        payload = json.dumps([
            {
                "DeviceID": "COM3",
                "Name": "USB 串行设备 (COM3)",
                "PNPDeviceID": "USB\\VID_303A&PID_1001&MI_00\\7&3261F339&0&0000",
                "Serial": "",
            },
        ])
        ports = parse_serial_ports(payload, dotii_serials=set())
        self.assertTrue(ports[0]["dotii"])


@unittest.skipUnless(os.name == "nt", "Windows tray host only")
class TrayHostTests(unittest.TestCase):
    def _host(self) -> bridge_app.TrayHost:
        host = object.__new__(bridge_app.TrayHost)
        host.hwnd = 123
        host.user32 = mock.Mock()
        host.user32.CreatePopupMenu.return_value = 456
        host.user32.AppendMenuW.return_value = True
        host.user32.DestroyMenu.return_value = True
        return host

    def test_context_menu_contains_about_entry(self) -> None:
        host = self._host()
        host._create_menu()
        labels = [
            call.args[3]
            for call in host.user32.AppendMenuW.call_args_list
            if len(call.args) >= 4 and call.args[3]
        ]
        self.assertIn("关于 Dotii 管理中心", labels)

    def test_about_dialog_reports_application_version(self) -> None:
        host = self._host()
        with mock.patch.object(bridge_app, "application_version", return_value="9.8.7"):
            host._show_about()
        dialog_text = host.user32.MessageBoxW.call_args.args[1]
        self.assertIn("版本 9.8.7", dialog_text)


class RuntimePathTests(unittest.TestCase):
    def test_application_version_reads_cmake_project_version(self) -> None:
        with tempfile.TemporaryDirectory(dir=BRIDGE.parent / ".codx") as temporary:
            root = Path(temporary)
            (root / "CMakeLists.txt").write_text(
                'set(PROJECT_VER "9.8.7")',
                encoding="utf-8",
            )
            with (
                mock.patch.object(runtime_paths, "application_root", return_value=root),
                mock.patch.object(runtime_paths, "bundle_root", return_value=root),
                mock.patch.object(runtime_paths, "is_frozen", return_value=False),
            ):
                self.assertEqual(runtime_paths.application_version(), "9.8.7")

    def test_application_version_falls_back_when_cmake_version_is_missing(self) -> None:
        with tempfile.TemporaryDirectory(dir=BRIDGE.parent / ".codx") as temporary:
            root = Path(temporary)
            (root / "CMakeLists.txt").write_text(
                "set(IDF_TARGET esp32s3)",
                encoding="utf-8",
            )
            with (
                mock.patch.object(runtime_paths, "application_root", return_value=root),
                mock.patch.object(runtime_paths, "bundle_root", return_value=root),
                mock.patch.object(runtime_paths, "is_frozen", return_value=False),
            ):
                self.assertEqual(runtime_paths.application_version(), "未知")


class MacOSPlatformAdapterTests(unittest.TestCase):
    def test_scaffold_uses_application_support_and_unix_tools(self) -> None:
        adapter = MacOSPlatformAdapter()
        self.assertEqual(adapter.runtime_folder().name, "Dotii")
        self.assertEqual(adapter.runtime_folder().parent.name, "Application Support")
        self.assertEqual(adapter.bundled_node_candidates(Path("/app"))[0].name, "node")

    def test_absolute_runtime_override_is_limited_to_explicit_development_runs(self) -> None:
        with tempfile.TemporaryDirectory(dir=BRIDGE.parent / ".codx") as temporary:
            with mock.patch.dict("os.environ", {"DOTII_RUNTIME_DIR": temporary}, clear=False):
                self.assertEqual(MacOSPlatformAdapter().runtime_folder(), Path(temporary))
        with mock.patch.dict("os.environ", {"DOTII_RUNTIME_DIR": "relative"}, clear=False):
            self.assertEqual(MacOSPlatformAdapter().runtime_folder().name, "Dotii")

    def test_finder_safe_tool_candidates_include_standard_mac_locations(self) -> None:
        adapter = MacOSPlatformAdapter()
        codex = adapter.codex_cli_candidates(Path("/runtime"), Path("/app"))
        node = adapter.bundled_node_candidates(Path("/app"))

        self.assertIn(Path("/opt/homebrew/bin/codex"), codex)
        self.assertIn(Path("/opt/homebrew/bin/node"), node)
        self.assertLess(codex.index(Path("/app/tools/codex/bin/codex")), codex.index(Path("/opt/homebrew/bin/codex")))

    def test_app_bundle_separates_resources_and_helper_executables(self) -> None:
        with tempfile.TemporaryDirectory(dir=BRIDGE.parent / ".codx") as temporary:
            contents = Path(temporary) / "Dotii Management Center.app" / "Contents"
            executable = contents / "Resources" / "DotiiBridgeRuntime" / "DotiiBridge"
            with (
                mock.patch.object(runtime_paths.sys, "platform", "darwin"),
                mock.patch.object(runtime_paths.sys, "executable", str(executable)),
                mock.patch.object(runtime_paths.sys, "frozen", True, create=True),
            ):
                self.assertEqual(runtime_paths.application_root(), contents / "Resources")
                self.assertEqual(
                    runtime_paths.sibling_executable("DotiiManagementCenter"),
                    contents / "MacOS" / "DotiiManagementCenter",
                )

    def test_platform_capabilities_match_implemented_macos_boundaries(self) -> None:
        adapter = MacOSPlatformAdapter()
        self.assertFalse(adapter.startup_available)
        self.assertTrue(adapter.bluetooth_available)
        self.assertFalse(adapter.bluetooth_pair_on_connect)
        self.assertTrue(adapter.serial_flash_available)
        self.assertFalse(adapter.startup_enabled())

    def test_packaged_login_item_uses_native_host_cli(self) -> None:
        with tempfile.TemporaryDirectory(dir=BRIDGE.parent / ".codx") as temporary:
            macos = Path(temporary) / "Dotii.app" / "Contents" / "MacOS"
            macos.mkdir(parents=True)
            bridge = macos.parent / "Resources" / "DotiiBridgeRuntime" / "DotiiBridge"
            bridge.parent.mkdir(parents=True)
            bridge.touch()
            host = macos / "DotiiManagementCenter"
            host.touch()
            responses = [
                mock.Mock(returncode=0, stdout="enabled\n", stderr=""),
                mock.Mock(returncode=0, stdout="enabled\n", stderr=""),
            ]
            with (
                mock.patch.object(sys, "executable", str(bridge)),
                mock.patch("platforms.macos.subprocess.run", side_effect=responses) as run,
            ):
                adapter = MacOSPlatformAdapter()
                command = adapter.current_startup_command(
                    frozen=True,
                    executable=bridge,
                    app_script=Path("unused"),
                    management_center=host,
                )
                self.assertTrue(adapter.set_startup(True, command))

            self.assertEqual(run.call_args_list[0].args[0], [str(host.resolve()), "--login-item", "enable"])
            self.assertEqual(run.call_args_list[1].args[0], [str(host.resolve()), "--login-item", "status"])

    def test_macos_serial_ports_use_callout_devices_and_dotii_vid_pid(self) -> None:
        ports = parse_macos_ports([
            SimpleNamespace(
                device="/dev/tty.usbmodem2101", description="TTY twin",
                vid=0x303A, pid=0x1001,
            ),
            SimpleNamespace(
                device="/dev/cu.usbserial-other", description="Other USB",
                vid=0x1234, pid=0x5678,
            ),
            SimpleNamespace(
                device="/dev/cu.usbmodem2101", description="Dotii ESP32-S3",
                vid=0x303A, pid=0x1001,
            ),
        ])

        self.assertEqual([item["port"] for item in ports], [
            "/dev/cu.usbmodem2101", "/dev/cu.usbserial-other",
        ])
        self.assertTrue(ports[0]["dotii"])
        self.assertFalse(ports[1]["dotii"])
        self.assertEqual(ports[0]["pnp_id"], "USB VID:PID=303A:1001")

    def test_macos_serial_validation_rejects_tty_and_path_traversal(self) -> None:
        adapter = MacOSPlatformAdapter()
        self.assertTrue(adapter.valid_serial_port("/dev/cu.usbmodem2101"))
        self.assertFalse(adapter.valid_serial_port("/dev/tty.usbmodem2101"))
        self.assertFalse(adapter.valid_serial_port("/dev/cu../disk0"))
        self.assertFalse(adapter.valid_serial_port("COM6"))

    @unittest.skipUnless(BLEAK_AVAILABLE, "bleak is not installed on this host")
    def test_bluetooth_service_is_available_with_corebluetooth(self) -> None:
        with tempfile.TemporaryDirectory(dir=BRIDGE.parent / ".codx") as temporary:
            service = BluetoothBridge(Path(temporary), platform_adapter=MacOSPlatformAdapter())
            snapshot = service.snapshot()

            self.assertTrue(snapshot["available"])
            self.assertTrue(snapshot["dependency_ready"])
            self.assertEqual(snapshot["permission_state"], "unknown")


if __name__ == "__main__":
    unittest.main()
