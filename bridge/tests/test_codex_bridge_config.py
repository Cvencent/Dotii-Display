from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


BRIDGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE))

from codex_bridge import (  # noqa: E402
    default_dotii_config,
    packaged_startup_command,
    startup_command,
    validate_display_config,
    validate_dotii_config,
    validate_module_config,
    validate_snapshot,
)
from platforms.windows import WindowsPlatformAdapter  # noqa: E402


class CodexUiConfigTests(unittest.TestCase):
    def test_display_config_defaults_to_classic_and_accepts_dual_limit(self) -> None:
        self.assertEqual(validate_display_config(None)["codex_ui"], "classic")
        self.assertEqual(validate_display_config({"codex_ui": "dual_limit"})["codex_ui"], "dual_limit")

    def test_display_config_rejects_unknown_codex_ui(self) -> None:
        with self.assertRaises(ValueError):
            validate_display_config({"codex_ui": "replace-classic"})

    def test_display_config_accepts_screen_off_page_targets(self) -> None:
        for target in ("none", "custom", "dotii"):
            self.assertEqual(
                validate_display_config({"screen_off_page": target})["screen_off_page"],
                target,
            )

    def test_display_config_rejects_unknown_screen_off_page_target(self) -> None:
        with self.assertRaises(ValueError):
            validate_display_config({"screen_off_page": "bambu"})

    def test_startup_command_uses_absolute_paths_and_startup_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            pythonw = folder / "pythonw.exe"
            app_script = folder / "Dotii 管理中心" / "bridge_app.py"
            pythonw.touch()
            app_script.parent.mkdir()
            app_script.touch()

            with mock.patch("codex_bridge.current_platform", return_value=WindowsPlatformAdapter()):
                self.assertEqual(
                    startup_command(pythonw, app_script),
                    f'"{pythonw.resolve()}" -B "{app_script.resolve()}" --startup',
                )

    def test_packaged_startup_targets_management_center_not_bridge(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            management_center = Path(temporary) / "DotiiManagementCenter.exe"
            management_center.touch()
            with mock.patch("codex_bridge.current_platform", return_value=WindowsPlatformAdapter()):
                command = packaged_startup_command(management_center)
            self.assertEqual(command, f'"{management_center.resolve()}" --startup')
            self.assertNotIn("DotiiBridge.exe", command)

    def test_display_config_migrates_charging_timeouts_from_existing_pair(self) -> None:
        display = validate_display_config({
            "screen_off_timeout_seconds": 30,
            "sleep_timeout_seconds": 180,
        })
        self.assertEqual(display["charging_screen_off_timeout_seconds"], 30)
        self.assertEqual(display["charging_sleep_timeout_seconds"], 180)

    def test_display_config_keeps_independent_power_timeout_pairs(self) -> None:
        display = validate_display_config({
            "screen_off_timeout_seconds": 30,
            "sleep_timeout_seconds": 180,
            "charging_screen_off_timeout_seconds": 300,
            "charging_sleep_timeout_seconds": 0,
        })
        self.assertEqual(display["screen_off_timeout_seconds"], 30)
        self.assertEqual(display["sleep_timeout_seconds"], 180)
        self.assertEqual(display["charging_screen_off_timeout_seconds"], 300)
        self.assertEqual(display["charging_sleep_timeout_seconds"], 0)

    def test_display_config_rejects_invalid_charging_timeout_order(self) -> None:
        with self.assertRaises(ValueError):
            validate_display_config({
                "charging_screen_off_timeout_seconds": 300,
                "charging_sleep_timeout_seconds": 60,
            })


if __name__ == "__main__":
    unittest.main()
