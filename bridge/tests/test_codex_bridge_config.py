from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


BRIDGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE))

from codex_bridge import (  # noqa: E402
    WAKE_WORD_MAX,
    packaged_startup_command,
    startup_command,
    validate_display_config,
    validate_xiaozhi_config,
    wake_word_list_to_wire,
)
from platforms.windows import WindowsPlatformAdapter  # noqa: E402


class CodexUiConfigTests(unittest.TestCase):
    def test_display_config_defaults_to_classic_and_accepts_dual_limit(self) -> None:
        self.assertEqual(validate_display_config(None)["codex_ui"], "classic")
        self.assertEqual(validate_display_config({"codex_ui": "dual_limit"})["codex_ui"], "dual_limit")

    def test_display_config_rejects_unknown_codex_ui(self) -> None:
        with self.assertRaises(ValueError):
            validate_display_config({"codex_ui": "replace-classic"})

    def test_display_config_keeps_black_screen_and_migrates_removed_pages(self) -> None:
        self.assertEqual(
            validate_display_config({"screen_off_page": "none"})["screen_off_page"],
            "none",
        )
        for target in ("custom", "dotii"):
            self.assertEqual(
                validate_display_config({"screen_off_page": target})["screen_off_page"],
                "none",
            )

    def test_display_config_rejects_unknown_screen_off_page_target(self) -> None:
        with self.assertRaises(ValueError):
            validate_display_config({"screen_off_page": "unknown"})

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


    def test_display_defaults_keep_a_docked_device_awake(self) -> None:
        display = validate_display_config(None)

        self.assertEqual(display["charging_screen_off_timeout_seconds"], 60)
        self.assertEqual(display["charging_sleep_timeout_seconds"], 0)
        self.assertEqual(display["sleep_timeout_seconds"], 300)


class WakeWordConfigTests(unittest.TestCase):
    """The device recognises custom wake words through MultiNet, which takes
    pinyin. These cover the conversion and the list contract."""

    def test_single_legacy_wake_word_becomes_a_one_item_list(self) -> None:
        config = validate_xiaozhi_config({"wake_word": "\u4f60\u597d\u5c0f\u667a"})

        self.assertEqual(config["wake_words"], ["\u4f60\u597d\u5c0f\u667a"])
        self.assertEqual(config["wake_word"], "\u4f60\u597d\u5c0f\u667a")

    def test_three_custom_words_are_accepted_and_packed_as_pinyin(self) -> None:
        words = ["\u4f60\u597d\u5c0f\u667a", "\u5c0f\u571f\u8c46", "\u5f00\u706f"]
        config = validate_xiaozhi_config({"wake_words": words})

        self.assertEqual(config["wake_words"], words)
        # Pinyin is what the recogniser consumes; the words stay readable in
        # the stored config so the admin page can show them back.
        self.assertEqual(
            wake_word_list_to_wire(config["wake_words"]),
            "ni hao xiao zhi;xiao tu dou;kai deng",
        )

    def test_more_than_three_words_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            validate_xiaozhi_config({"wake_words": ["a", "b", "c", "d"]})

    def test_blank_entries_are_dropped_rather_than_stored(self) -> None:
        config = validate_xiaozhi_config({"wake_words": ["\u5c0f\u571f\u8c46", "  ", ""]})

        self.assertEqual(config["wake_words"], ["\u5c0f\u571f\u8c46"])

    def test_an_all_blank_list_falls_back_to_the_default_word(self) -> None:
        # An empty list must never leave the device without a way to be woken.
        config = validate_xiaozhi_config({"wake_words": []})

        self.assertEqual(config["wake_words"], ["\u4f60\u597d\u5c0f\u667a"])

    def test_english_words_pass_through_unchanged(self) -> None:
        config = validate_xiaozhi_config({"wake_words": ["hey dotii"]})

        self.assertEqual(wake_word_list_to_wire(config["wake_words"]), "hey dotii")

    def test_wire_format_is_capped_at_the_supported_word_count(self) -> None:
        self.assertEqual(WAKE_WORD_MAX, 3)
        # Defensive: even if a caller bypasses validation, the wire string
        # carries no more than the device can actually register.
        wire = wake_word_list_to_wire(["a", "b", "c", "d"])
        self.assertEqual(wire, "a;b;c")


if __name__ == "__main__":
    unittest.main()
